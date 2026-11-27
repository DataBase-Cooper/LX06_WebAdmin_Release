# Official SIWC open-source dynamic registration; no borrowed client secret.
PROVIDER=chatgpt
TOKEN_URL=https://auth.openai.com/api/accounts/oauth/token
BASE_URL=https://api.openai.com/v1
provider_preconnect() { HTTP_WARM_URL="$BASE_URL/models"; }
ACCOUNT_LOGIN_USING_CALLBACK=1
REQUIRED_SCOPE=chatgpt.tokens.use.direct
MODEL_LIST_KEY=models
MODEL_ID_KEY=slug
MODEL_LABEL_KEY=display_name
MODEL_FILTER_KEY=visibility
MODEL_FILTER_VALUE=list
# Keep the account catalog first, then merge the standard data/id fallback.
MODEL_LIST_SPECS='models|slug|display_name|visibility|list data|id|id|-|- data|slug|display_name|-|-'
provider_headers() { :; }
provider_request_options() {
    # Responses uses reasoning.effort, not Chat Completions reasoning_effort.
    # Sol supports low (not none/minimal); explicitly avoid an implicit default.
    # Verified: https://developers.openai.com/api/docs/models/gpt-6.1-sol
    # https://developers.openai.com/api/docs/guides/reasoning
    # Keep other account/custom model IDs untouched until separately verified.
    case "$1" in gpt-6.1-sol) printf ',"reasoning":{"effort":"low"}';; esac
}
refresh_form() {
    put resource "$BASE_URL"
    form grant_type "$WORK/grant" client_id "$WORK/client" refresh_token "$WORK/refresh" resource "$WORK/resource"
}
prepare_host_id() {
    # SIWC requires a URI identifier, not a bare base64url random value.
    # Generate UUIDv4 on the speaker once; neither DID nor MAC leaves the device.
    # https://developers.openai.com/siwc/token-sharing-open-source
    [ ! -L "$ACCOUNT/host-id" ] || fail '主机标识文件异常。'
    host_id=$(cat "$ACCOUNT/host-id" 2>/dev/null || :)
    uuid_pattern='^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
    if ! printf '%s\n' "$host_id" | grep -Eq "$uuid_pattern"; then
        # Repair the rejected pre-registration experimental value only. Never
        # silently change the identity of an already authorized installation.
        [ ! -f "$RECORD" ] || fail '已授权账户的主机标识异常；保留账户，请检查标识文件。'
        host_id="urn:uuid:$(cat /proc/sys/kernel/random/uuid)"
        printf '%s\n' "$host_id" | grep -Eq "$uuid_pattern" || fail '无法生成有效的本机 UUID。'
        pending=$(mktemp "$ACCOUNT/.host-id.XXXXXX")
        printf '%s' "$host_id" > "$pending"
        chmod 600 "$pending"; mv -f "$pending" "$ACCOUNT/host-id"; pending=
    fi
    put host "$host_id"
}
login_request() {
    prepare_host_id
    client=dynamic_agent_client
    if [ -f "$RECORD" ]; then client=$(get client_id "$RECORD"); fi
    put client "$client"
    "$TOOL" random > "$WORK/state"; "$TOOL" random > "$WORK/nonce"; "$TOOL" random > "$WORK/verifier"
    "$TOOL" pkce < "$WORK/verifier" | tr -d '\n' > "$WORK/challenge"
    # The registered loopback URI is shared by both methods. The default
    # two-step login does not require a listener on the user's computer: the
    # user pastes the returned address into this speaker's private extension.
    put redirect http://127.0.0.1:1455/auth/callback
    put resource "$BASE_URL"; put method S256; put response code
    put scope 'openid profile email offline_access resource.invoke chatgpt.tokens.use.direct'
    # Trim random output once, preserving exactly the same state/nonce values.
    for f in state nonce verifier; do v=$(cat "$WORK/$f"); put "$f" "$v"; done
    set -- client_id "$WORK/client" ext_agent_host_id "$WORK/host" redirect_uri "$WORK/redirect" resource "$WORK/resource" response_type "$WORK/response" scope "$WORK/scope" state "$WORK/state" nonce "$WORK/nonce" code_challenge_method "$WORK/method" code_challenge "$WORK/challenge"
    if [ "$client" = dynamic_agent_client ]; then put app 'LX06 XiaoAI WebAdmin'; set -- "$@" agent_name_hint "$WORK/app"; fi
    query=$("$TOOL" form "$@")
    put link "https://auth.openai.com/api/accounts/authorize?$query"
}
login_exchange() {
    error=$(get error "$WORK/callback" 2>/dev/null || :)
    [ -z "$error" ] || fail '官方授权未完成，原账户保留。'
    issued=$(get client_id "$WORK/callback" 2>/dev/null || :)
    if [ "$client" = dynamic_agent_client ]; then
        case "$issued" in oaiapp_*) :;; *) fail '平台没有返回有效的本应用注册 ID。';; esac
    else
        [ -z "$issued" ] || [ "$issued" = "$client" ] || fail '回调注册 ID 不匹配。'
        issued=$client
    fi
    put client "$issued"
    get code "$WORK/callback" > "$WORK/code" || fail '授权码缺失。'
    put grant authorization_code
    form grant_type "$WORK/grant" client_id "$WORK/client" code "$WORK/code" code_verifier "$WORK/verifier" redirect_uri "$WORK/redirect" resource "$WORK/resource"
    http POST "$TOKEN_URL" "$WORK/token" || fail '官方令牌交换失败，原账户保留。'
    get id_token "$WORK/token" > "$WORK/jwt" || fail '官方身份凭据缺失。'
    http GET https://auth.openai.com/.well-known/jwks.json "$WORK/jwks" || fail '无法获取官方签名公钥。'
    "$TOOL" jwt-verify "$WORK/jwt" "$WORK/jwks" https://auth.openai.com "$issued" "$WORK/nonce" > "$WORK/identity" || fail '官方身份签名或登录绑定校验失败。'
    get sub "$WORK/identity" > "$WORK/subject"
    if [ -f "$RECORD" ]; then
        old=$(get subject "$RECORD"); new=$(cat "$WORK/subject")
        [ "$old" = "$new" ] || fail '本次登录不是已选账户；切换账号请先退出登录。'
    fi
    SAVING_REFRESH=0; save_tokens
    account_forget_model_options
    ACCOUNT_MODELS_COMMITTED=1
    if (models); then
        ui_settings configure 'ChatGPT 登录成功，已读取账户可用模型。'
    else
        ui_settings configure 'ChatGPT 登录成功，暂未取得模型目录。请点击更新模型列表。'
    fi
}

# Login secrets live only in bounded, root-private TMP. There is no detached
# listener/job to occupy the extension while the user signs in on a phone/PC.
# Each new login invalidates the old pending state; completion consumes it
# before exchanging the one-use code. Existing credentials change only after
# the same PKCE, nonce, issuer, audience and subject checks as automatic login.
LOGIN_PENDING=/tmp/lx06-chatgpt-login
login_private_dir() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 1
    set -- $(LC_ALL=C ls -ldn "$1" 2>/dev/null)
    [ "${1:-}" = drwx------ ] && [ "${3:-}" = 0 ]
}
login_private_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    set -- $(LC_ALL=C ls -ln "$1" 2>/dev/null)
    [ "${1:-}" = -rw------- ] && [ "${2:-}" = 1 ] && [ "${3:-}" = 0 ]
}
login_pending_clear() {
    if [ -e "$LOGIN_PENDING" ] || [ -L "$LOGIN_PENDING" ]; then
        login_private_dir "$LOGIN_PENDING" || fail '临时授权目录异常，原账户保留。'
        for f in state nonce verifier client redirect resource created link; do rm -f "$LOGIN_PENDING/$f"; done
        rmdir "$LOGIN_PENDING" || fail '无法清理上次临时授权，原账户保留。'
    fi
}
login_pending_load() {
    login_private_dir "$LOGIN_PENDING" || fail '没有待完成的登录，请先点击登录账户。'
    for f in state nonce verifier client redirect resource created link; do
        login_private_file "$LOGIN_PENDING/$f" || fail '临时授权记录异常，请重新点击登录账户。'
    done
    created=$(cat "$LOGIN_PENDING/created")
    case "$created" in ''|*[!0-9]*) fail '临时授权记录无效，请重新登录。';; esac
    read -r uptime rest < /proc/uptime
    now=${uptime%%.*}
    if [ "$created" -gt "$now" ] || [ "$(( now - created ))" -gt 600 ]; then
        login_pending_clear
        fail '本次授权已过期，请重新点击登录账户。'
    fi
    for f in state nonce verifier client redirect resource link; do cp "$LOGIN_PENDING/$f" "$WORK/$f"; done
    client=$(cat "$WORK/client")
}
login() {
    login_request
    login_pending_clear
    mkdir -m 700 "$LOGIN_PENDING" || fail '无法保存临时授权，请稍后重试。'
    for f in state nonce verifier client redirect resource link; do cp "$WORK/$f" "$LOGIN_PENDING/$f"; chmod 600 "$LOGIN_PENDING/$f"; done
    read -r uptime rest < /proc/uptime
    printf '%s' "${uptime%%.*}" > "$LOGIN_PENDING/created"
    chmod 600 "$LOGIN_PENDING/created"
    put message '打开授权页面登录。跳转后若显示无法访问，请复制地址栏完整地址，粘贴到“授权回调地址”并点“完成登录”。10 分钟内有效，无需 SSH 或电脑转发。'
    put ui_step authorize
    "$TOOL" object message "$WORK/message" link "$WORK/link" step "$WORK/ui_step"
}
provider_ui_pending() {
    if [ "${1:-}" = available ]; then [ -e "$LOGIN_PENDING" ] || [ -L "$LOGIN_PENDING" ]; return; fi
    if ! (login_pending_load); then
        ui_settings login '授权记录已失效，请重新登录；原账户保留。';return
    fi
    ui_settings authorize
    put message '继续完成官网授权，将跳转后的完整地址粘贴到下方。授权有效期内关闭窗口不会丢失进度。'
    put ui_step authorize
    "$TOOL" object step "$WORK/ui_step" message "$WORK/message" link "$WORK/link"
}
login_automatic() {
    login_request
    login_pending_clear
    "$TOOL" callback 1455 /auth/callback "$WORK/state" "$WORK/ready" > "$WORK/callback" &
    CALLBACK_PID=$!
    tries=0
    while [ ! -e "$WORK/ready" ]; do
        kill -0 "$CALLBACK_PID" 2>/dev/null || fail '回调端口无法启动，请关闭上次登录。'
        tries=$((tries + 1)); [ "$tries" -le 4 ] || fail '回调准备超时。'; sleep 1
    done
    put message '临时自动回调模式：须先在登录设备建立到音箱 1455 端口的 SSH 转发。没有转发时请改用“登录账户”和“完成登录”。'
    "$TOOL" object message "$WORK/message" link "$WORK/link"
    wait "$CALLBACK_PID" || fail '自动回调已超时，原账户保留；可改用两步登录。'
    CALLBACK_PID=
    login_exchange
}
complete_login() {
    login_pending_load
    callback_url=$(field auth_callback)
    [ "${#callback_url}" -le 2048 ] || fail '回调地址过长，请重新复制地址栏完整地址。'
    case "$callback_url" in http://127.0.0.1:1455/auth/callback\?*) :;; *) fail '请粘贴官网跳转后的完整回调地址（以 http://127.0.0.1:1455/auth/callback? 开头）。';; esac
    # A private curl config carries the secret URL, never argv. Reject config
    # delimiters, fragments, controls and non-URI bytes before writing it.
    clean_url=$(printf '%s' "$callback_url" | LC_ALL=C tr -cd 'A-Za-z0-9._~:/?=&%+\055')
    [ "$clean_url" = "$callback_url" ] || fail '回调地址含无效字符，请重新复制地址栏完整地址。'
    unset clean_url
    printf 'url = "%s"\n' "$callback_url" > "$WORK/callback-curl"
    unset callback_url
    # Reuse the already shipped strict URL decoder/state validator. The HTTP
    # request is entirely speaker-loopback; no external relay or extra helper.
    "$TOOL" callback 1455 /auth/callback "$WORK/state" "$WORK/ready" > "$WORK/callback" &
    CALLBACK_PID=$!
    tries=0
    while [ ! -e "$WORK/ready" ]; do
        kill -0 "$CALLBACK_PID" 2>/dev/null || fail '音箱回调端口被占用，请结束自动回调后重试。'
        tries=$((tries + 1)); [ "$tries" -le 40 ] || fail '回调校验准备超时，请稍后重试。'; sleep .1
    done
    callback_status=$(curl --silent --noproxy '*' --proto '=http' --connect-timeout 2 --max-time 4 --config "$WORK/callback-curl" --output "$WORK/callback-response" --write-out '%{http_code}' 2>/dev/null) || fail '音箱回调校验失败，请稍后重试。'
    [ "$callback_status" = 200 ] || fail '回调地址与本次登录不匹配，请使用最新授权页面返回的完整地址。'
    wait "$CALLBACK_PID" || fail '授权校验失败，原账户保留。'
    CALLBACK_PID=
    login_pending_clear
    login_exchange
}
account_status() {
    if [ -d "$LOGIN_PENDING" ] && [ ! -L "$LOGIN_PENDING" ]; then
        login_pending_load
        put message '授权待完成：登录后复制回调地址到“授权回调地址”，点击“完成登录”。原账户仍保留。'
        "$TOOL" object message "$WORK/message" link "$WORK/link"
    elif [ -f "$RECORD" ]; then public 'ChatGPT 已登录。可获取模型或进行文字测试。'
    else public '尚未登录。点“登录账户”，官网授权后提交回调地址即可，无需电脑转发。'; fi
}
logout() {
    login_pending_clear
    rm -f "$RECORD"
    public '已删除音箱本地登录凭据及待完成授权。若需撤销平台授权，请到平台账户设置操作。'
}
provider_uninstall() { login_pending_clear; }
