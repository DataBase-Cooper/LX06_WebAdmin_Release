# Experimental account login only. This source is appended to account-common.sh.
# Website endpoints are NOT the paid API, OAuth or a promised stable interface.
# Do not add voice/wake markers until inference mapping has been implemented.
PROVIDER=deepseek
BASE_URL=https://chat.deepseek.com
provider_preconnect() { HTTP_WARM_URL="$BASE_URL/"; }
SMS_COOLDOWN=/tmp/lx06-deepseek-sms-cooldown
DS_DIAGNOSTIC=/tmp/lx06-deepseek-last-request.json
ds_stage=request

provider_headers() {
    printf '%s\n' 'header = "Accept: application/json"' \
        'header = "Content-Type: application/json"' \
        'header = "Origin: https://chat.deepseek.com"' \
        'header = "Referer: https://chat.deepseek.com/"' \
        'header = "x-client-bundle-id: com.deepseek.chat"' \
        'header = "x-client-platform: web"' \
        'header = "x-client-version: 2.7.0"' \
        'header = "x-client-locale: zh_CN"' \
        'header = "x-client-timezone-offset: 28800"' \
        'user-agent = "LX06-XiaoAI-WebAdmin/DeepSeek-login-test (Linux; armv7l)"'
}

# Never emit the upstream message/body: it may contain account identifiers or
# credentials. Numeric codes are enough to diagnose protocol changes privately.
ds_failure() {
    # model-voice consumes stdout as NDJSON; a failed request's public message
    # is not an answer delta. Mirror only our own fixed text into script logs
    # so a rejected token does not look like an unexplained silent response.
    printf 'DeepSeek: %s\n' "$1" >&2
    if [ "${DS_VOICE_REQUEST:-0}" = 1 ];then
        put failure-message "$1"
        "$TOOL" object error "$WORK/failure-message"
        exit 1
    fi
    fail "$1"
}

# Identity validity and chat permission are separate. Never discard a valid
# token just because the server has muted this account. Only the server's
# structured flags/time and our fixed messages enter UI/logs, never raw bodies.
ds_mute_notice() {
    DS_NOTICE='DeepSeek 账号已被禁言，暂时无法对话，请到官网查看原因。'
    mute_until=${1%%.*}
    case "$mute_until" in ''|*[!0-9]*|???????????*) return 0;; esac
    if [ "$mute_until" -gt "$(date +%s)" ];then
        mute_date=$(TZ=CST-8 date -d "@$mute_until" '+%Y-%m-%d %H:%M' 2>/dev/null || :)
        [ -z "$mute_date" ] || DS_NOTICE="DeepSeek 账号已被禁言，服务器给出的截止时间为北京时间 $mute_date，请到官网查看原因。"
    fi
}

ds_account_notice() {
    DS_NOTICE=
    if get chat "$WORK/user" > "$WORK/chat-permission" 2>/dev/null;then
        muted=$(get is_muted "$WORK/chat-permission" 2>/dev/null || :)
        case "$muted" in 1|true)
            ds_mute_notice "$(get mute_until "$WORK/chat-permission" 2>/dev/null || :)";;
        esac
    fi
}
ds_failed() {
    # Observed completion refusal: HTTP 200 / code=0 / biz_code=5 with
    # biz_data.is_muted=1. Business code 5 alone must not imply a ban on other
    # endpoints. Preserve both real codes instead of inventing SDK code 50006.
    if [ "${target:-}" = /api/v0/chat/completion ] &&
       get biz_data "$WORK/error-envelope" > "$WORK/error-business" 2>/dev/null;then
        case "$(get is_muted "$WORK/error-business" 2>/dev/null || :)" in
            1|true)
                ds_mute_notice "$(get mute_until "$WORK/error-business" 2>/dev/null || :)"
                ds_failure "$DS_NOTICE";;
        esac
    fi
    case "${upstream_code:-}" in
        50006)
            until_value=$(get mute_until "$WORK/error-envelope" 2>/dev/null || :)
            ds_mute_notice "$until_value";ds_failure "$DS_NOTICE";;
        40012) ds_failure 'DeepSeek 账号已被平台封禁，暂时无法对话，请到官网查看原因。';;
        40029) ds_failure 'DeepSeek 限制了当前网络的访问，请检查网络或到官网查看提示。';;
        40002) ds_failure 'DeepSeek 未收到有效登录凭据，请在脚本扩展中检查登录。';;
    esac
    # Observed users/current + chat_session/create response on 2026-10-31:
    # code=40003, "Authorization Failed (invalid token)". No stored password
    # or refresh credential exists; preserve the record and ask for re-login.
    if [ "${upstream_code:-}" = 40003 ];then
        case "${target:-}" in
            /api/v0/users/current|/api/v0/chat_session/*|/api/v0/chat/*)
                ds_failure 'DeepSeek 登录凭据已失效，请在脚本扩展中重新登录；原记录保留。';;
        esac
    fi
    case "${status:-}" in
        429) ds_failure 'DeepSeek 当前限流（HTTP 429），请稍后再试；原登录状态保留。';;
        401) ds_failure 'DeepSeek 未接受登录凭据，请检查账号密码或重新登录；原记录保留。';;
        403) ds_failure 'DeepSeek 拒绝此请求，可能需要在官网完成验证；本脚本不处理网页验证。';;
        400|422) ds_failure 'DeepSeek 拒绝了脚本的请求格式，请更新测试脚本；无需反复修改手机号。';;
        404) ds_failure 'DeepSeek 登录接口不可用，需要更新脚本。';;
        2??)
            if [ -n "${upstream_code:-}" ] && [ "$upstream_code" != 0 ];then
                ds_failure "DeepSeek 拒绝此请求（平台代码 $upstream_code）；原记录保留。"
            fi
            case "$ds_stage" in
                login_user) ds_failure 'DeepSeek 登录响应缺少有效账户或凭据，请更新测试脚本；原记录保留。';;
                current_response) ds_failure 'DeepSeek 登录后的账户复核未通过；原记录保留，请稍后再试。';;
                *) ds_failure "DeepSeek 未接受此操作（业务代码 ${business_code:-unknown}）；原记录保留。";;
            esac;;
        *) ds_failure '无法连接 DeepSeek，请检查网络或稍后重试；原记录保留。';;
    esac
}

ds_http_error() {
    # Keep only numeric codes and the public route in bounded TMP. Never save
    # the raw reply, phone, password, OTP, token or headers in diagnostic logs.
    upstream_code=$(get code "$1" 2>/dev/null || :)
    business_code=
    if get data "$1" > "$WORK/error-envelope" 2>/dev/null;then
        business_code=$(get biz_code "$WORK/error-envelope" 2>/dev/null || :)
    fi
    for name in upstream_code business_code;do
        if [ "$name" = upstream_code ];then value=$upstream_code;else value=$business_code;fi
        case "$value" in *[!0-9-]*|?????????*) value=unknown;; esac
        put "$name" "$value"
    done
    upstream_code=$(cat "$WORK/upstream_code")
    business_code=$(cat "$WORK/business_code")
    printf 'deepseek-error: stage=%s route=%s http=%s code=%s biz_code=%s\n' \
        "$ds_stage" "$target" "${status:-0}" "${upstream_code:-unknown}" "${business_code:-unknown}" >&2
    put http "${status:-0}";put route "$target";put stage "$ds_stage"
    if [ ! -L "$DS_DIAGNOSTIC" ];then
        "$TOOL" object http "$WORK/http" route "$WORK/route" stage "$WORK/stage" code "$WORK/upstream_code" biz_code "$WORK/business_code" > "$WORK/diagnostic"
        mv -f "$WORK/diagnostic" "$DS_DIAGNOSTIC"
    fi
    case "$upstream_code" in
        40300|40301|40303|40304) fail 'DeepSeek 未接受访客校验或要求官网验证；请更新脚本或在官网完成验证，不要反复提交验证码。';;
    esac
    ds_failed
}

# Public, per-target computational challenge used by the official web client.
# This does not solve human CAPTCHA. No phone/OTP goes to this endpoint.
ds_guest() {
    mv "$WORK/form" "$WORK/login-form"
    put guest-target "$target"
    "$TOOL" object target_path "$WORK/guest-target" > "$WORK/form"
    provider_headers > "$WORK/headers"
    ds_stage=guest_challenge
    http POST "$BASE_URL/api/v0/users/create_guest_challenge" "$WORK/guest" "$WORK/headers" || ds_http_error "$WORK/guest"
    [ "$(get code "$WORK/guest" 2>/dev/null || :)" = 0 ] || ds_http_error "$WORK/guest"
    get data "$WORK/guest" > "$WORK/guest-data" || ds_http_error "$WORK/guest"
    [ "$(get biz_code "$WORK/guest-data" 2>/dev/null || :)" = 0 ] || ds_http_error "$WORK/guest"
    get biz_data "$WORK/guest-data" > "$WORK/guest-business" &&
        get guest_challenge "$WORK/guest-business" > "$WORK/puzzle" || fail 'DeepSeek 未返回访客校验参数，请更新脚本。'
    [ "$(get algorithm "$WORK/puzzle" 2>/dev/null || :)" = DeepSeekHashV1 ] || fail 'DeepSeek 访客校验算法已变化，请更新脚本。'
    ds_solve
    mv "$WORK/login-form" "$WORK/form"
    ds_stage=request
}

# Shared bounded primitive for guest-login and authenticated-chat challenges.
ds_solve() {
    [ "$(get algorithm "$WORK/puzzle" 2>/dev/null || :)" = DeepSeekHashV1 ] || fail 'DeepSeek 校验算法已变化，请更新脚本。'
    expiry=$(get expire_at "$WORK/puzzle" 2>/dev/null || :)
    difficulty=$(get difficulty "$WORK/puzzle" 2>/dev/null || :)
    case "$expiry" in ''|*[!0-9]*) fail 'DeepSeek 校验有效期无效。';; esac
    [ "${#expiry}" = 13 ] || fail 'DeepSeek 校验有效期格式已变化。'
    expires=${expiry%???};now=$(date +%s)
    [ "$expires" -gt "$now" ] && [ "$expires" -le "$((now+600))" ] || fail 'DeepSeek 校验已过期或音箱时间不正确，请稍后再试。'
    case "$difficulty" in ''|*[!0-9]*|???????*) fail 'DeepSeek 校验计算量无效。';; esac
    [ "$difficulty" -gt 0 ] && [ "$difficulty" -le 200000 ] || fail 'DeepSeek 校验超过音箱计算预算，请使用官网。'
    get salt "$WORK/puzzle" > "$WORK/salt" && get challenge "$WORK/puzzle" > "$WORK/challenge" || fail 'DeepSeek 校验参数缺失。'
    put expiry "$expiry";put difficulty "$difficulty"
    ds_sealer
    answer=$("$WORK/auth-seal" --hash23-solve "$WORK/salt" "$WORK/expiry" "$WORK/difficulty" "$WORK/challenge") || fail '音箱未能在计算预算内完成 DeepSeek 访客校验，请稍后再试。'
    case "$answer" in ''|*[!0-9]*) fail 'DeepSeek 校验计算结果无效。';; esac
    { printf '{"salt":';"$TOOL" quote < "$WORK/salt";printf ',"answer":%s}' "$answer"; } > "$WORK/proof-json"
    "$WORK/auth-seal" --base64 < "$WORK/proof-json" > "$WORK/guest-proof" || fail 'DeepSeek 校验编码失败。'
}

# Match the current official request envelope. This is encryption, not a
# CAPTCHA/verification bypass. Extracted helper lives only in this action's TMP.
ds_post() {
    target=$1; result=$2
    rm -f "$WORK/guest-proof"
    case "$target" in
        /api/v0/users/create_sms_verification_code|/api/v0/users/login_by_mobile_sms) ds_guest;;
    esac
    ds_sealer
    "$WORK/auth-seal" "$WORK/auth-public.der" "$WORK/sealed" "$WORK/encryption-key" < "$WORK/form" || fail '登录请求加密失败，请更新脚本。'
    mv -f "$WORK/sealed" "$WORK/form"
    provider_headers > "$WORK/headers"
    printf 'header = "x-request-encryption-key: %s"\n' "$(cat "$WORK/encryption-key")" >> "$WORK/headers"
    if [ -f "$WORK/guest-proof" ];then
        printf 'header = "X-DS-Guest-PoW-Response: %s"\n' "$(cat "$WORK/guest-proof")" >> "$WORK/headers"
    fi
    http POST "$BASE_URL$target" "$result" "$WORK/headers" || ds_http_error "$result"
    rm -f "$WORK/form"
}

ds_device() {
    [ ! -L "$ACCOUNT/device-id" ] || fail '设备标识文件异常。'
    if [ ! -f "$ACCOUNT/device-id" ];then
        pending=$(mktemp "$ACCOUNT/.device.XXXXXX")
        "$TOOL" random > "$pending"
        chmod 600 "$pending";mv -f "$pending" "$ACCOUNT/device-id";pending=
    fi
    device=$(cat "$ACCOUNT/device-id")
    token_safe "$device" && [ "${#device}" = 43 ] || fail '设备标识无效，请卸载测试脚本后重新上传。'
    put device "$device"; put os web;put locale zh_CN;put region '[unset]'
}

ds_mobile() {
    account=$(field account);area=$(field area_code)
    case "$area" in +*) digits=${area#+};; *) fail '手机号区号需以 + 开头，例如 +86。';; esac
    case "$digits" in ''|*[!0-9]*|????*) fail '手机号区号不正确。';; esac
    case "$account" in "$area"*) account=${account#"$area"};; esac
    case "$account" in ''|*[!0-9]*) fail '请输入手机号，不能填写邮箱。';; esac
    [ "${#account}" -ge 5 ] && [ "${#account}" -le 15 ] || fail '手机号长度不正确。'
    put mobile "$account";put area "$area"
}

ds_sms_cooldown() {
    now=$(date +%s)
    [ ! -L "$SMS_COOLDOWN" ] || fail '短信冷却状态异常。'
    if [ -f "$SMS_COOLDOWN" ];then
        until=$(cat "$SMS_COOLDOWN")
        case "$until" in ''|*[!0-9]*|????????????*) until=0;; esac
        if [ "$until" -gt "$now" ];then fail "请等待 $((until-now)) 秒后再发送验证码。";fi
    fi
}

ds_need_browser() {
    put message 'DeepSeek 要求官网验证，请点击扩展中的“打开官网”，手动完成验证并发送短信。可回到此处尝试验证码登录，但跨设备验证码是否可用仍需实测。'
    put link https://chat.deepseek.com/sign_in
    "$TOOL" object message "$WORK/message" link "$WORK/link"
    exit 1
}

send_sms() {
    ds_mobile
    [ "$area" = +86 ] && [ "${#account}" = 11 ] || fail '官网当前短信入口仅提供中国大陆 +86 手机号，请使用已注册的 11 位手机号。'
    ds_sms_cooldown
    # The UI calls this flow "mobileLogin", but transforms the HTTP scenario
    # to "login". Sending the UI label itself is rejected by the real service.
    ds_device;put scenario login
    "$TOOL" object mobile_number "$WORK/mobile" locale "$WORK/locale" device_id "$WORK/device" scenario "$WORK/scenario" > "$WORK/form"
    rm -f "$WORK/request" "$WORK/values"
    ds_post /api/v0/users/create_sms_verification_code "$WORK/sms-response"
    [ "$(get code "$WORK/sms-response" 2>/dev/null || :)" = 0 ] || ds_http_error "$WORK/sms-response"
    get data "$WORK/sms-response" > "$WORK/sms-data" || ds_http_error "$WORK/sms-response"
    biz=$(get biz_code "$WORK/sms-data" 2>/dev/null || :)
    case "$biz" in
        0|1)
            wait_seconds=60
            if get biz_data "$WORK/sms-data" > "$WORK/sms-business" 2>/dev/null;then
                seconds=$(get send_window_secs "$WORK/sms-business" 2>/dev/null || :)
                case "$seconds" in ''|*[!0-9]*|????*) :;; *) [ "$seconds" -le 600 ] && [ "$seconds" -gt 60 ] && wait_seconds=$seconds;; esac
            fi
            printf '%s' "$(( $(date +%s) + wait_seconds ))" > "$WORK/cooldown"
            mv -f "$WORK/cooldown" "$SMS_COOLDOWN"
            if [ "$biz" = 0 ];then public 'DeepSeek 已接受短信发送请求，请查看手机，填写 6 位验证码后点击验证码登录。';
            else fail '短信发送过于频繁，请等待至少 60 秒后再试。';fi;;
        2) ds_need_browser;;
        3|6) fail 'DeepSeek 未接受手机号，请检查已注册的中国大陆手机号。';;
        98) fail 'DeepSeek 暂停了此通道的短信发送，请使用官网。';;
        *) ds_http_error "$WORK/sms-response";;
    esac
}

sms_login() {
    ds_mobile
    [ "$area" = +86 ] && [ "${#account}" = 11 ] || fail '验证码登录当前仅测试 +86 的 11 位手机号。'
    code=$(field sms_code)
    case "$code" in *[!0-9]*) fail '短信验证码必须是 6 位数字。';; esac
    [ "${#code}" = 6 ] || fail '短信验证码必须是 6 位数字。'
    put sms-code "$code";unset code
    ds_device
    "$TOOL" object mobile_number "$WORK/mobile" area_code "$WORK/area" sms_verification_code "$WORK/sms-code" device_id "$WORK/device" os "$WORK/os" locale "$WORK/locale" region "$WORK/region" > "$WORK/form"
    rm -f "$WORK/request" "$WORK/values" "$WORK/sms-code"
    public '正在验证短信验证码和账户身份…'
    ds_post /api/v0/users/login_by_mobile_sms "$WORK/login"
    [ "$(get code "$WORK/login" 2>/dev/null || :)" = 0 ] || ds_http_error "$WORK/login"
    get data "$WORK/login" > "$WORK/login-data" || ds_http_error "$WORK/login"
    biz=$(get biz_code "$WORK/login-data" 2>/dev/null || :)
    case "$biz" in
        0|1) ds_finish_login "$biz";;
        7) fail '短信验证码已过期，请重新获取。';;
        8) fail '短信验证码不正确，请检查后再试。';;
        9) fail '验证码尝试次数过多，请稍后再试。';;
        11) ds_need_browser;;
        *) ds_http_error "$WORK/login";;
    esac
}

# Decode the nested website envelope using the existing bounded JSON tool.
# Require BOTH business codes, a real identity and, on login, a usable token.
# No token/identity is ever returned to the extension or script logs.
ds_user() {
    response=$1
    [ "$(get code "$response" 2>/dev/null || :)" = 0 ] || return 1
    get data "$response" > "$WORK/envelope" || return 1
    [ "$(get biz_code "$WORK/envelope" 2>/dev/null || :)" = "${2:-0}" ] || return 1
    get biz_data "$WORK/envelope" > "$WORK/business" || return 1
    if ! get user "$WORK/business" > "$WORK/user" 2>/dev/null; then
        cp "$WORK/business" "$WORK/user"
    fi
    subject=$(get id "$WORK/user" 2>/dev/null || :)
    case "$subject" in ''|*[!a-zA-Z0-9._-]*) return 1;; esac
    [ "${#subject}" -le 128 ] || return 1
}

ds_auth() {
    token_safe "$access" && [ "${#access}" -le 2048 ] || fail 'DeepSeek 登录凭据格式无效，请重新登录。'
    printf 'header = "Authorization: Bearer %s"\n' "$access" > "$WORK/auth"
    provider_headers >> "$WORK/auth"
}

ds_verify() {
    expected_subject=$1
    ds_auth
    ds_stage=current_response;target=/api/v0/users/current
    http GET "$BASE_URL$target" "$WORK/current" "$WORK/auth" || ds_http_error "$WORK/current"
    ds_user "$WORK/current" || ds_http_error "$WORK/current"
    [ "$subject" = "$expected_subject" ] || fail 'DeepSeek 返回了不同的账户身份，原登录记录未覆盖。'
    # Some website versions rotate the token on users/current. Preserve the
    # original only if no replacement field exists; reject a malformed one.
    if get token "$WORK/user" > "$WORK/rotated" 2>/dev/null; then
        access=$(cat "$WORK/rotated")
        token_safe "$access" && [ "${#access}" -le 2048 ] || ds_failed
    fi
}

ds_save() {
    put access "$access"; put subject "$subject"; put verified "$(date +%s)"
    pending=$(mktemp "$ACCOUNT/.credentials.XXXXXX")
    "$TOOL" object access_token "$WORK/access" subject "$WORK/subject" verified_at "$WORK/verified" > "$pending" || fail '无法保存 DeepSeek 登录凭据。'
    chmod 600 "$pending"
    mv -f "$pending" "$RECORD"
    pending=
}

login() {
    account=$(field account); password=$(field password); area=$(field area_code)
    [ -n "$account" ] && [ "${#account}" -le 254 ] || fail '请输入 DeepSeek 邮箱或手机号。'
    [ -n "$password" ] && [ "${#password}" -le 256 ] || fail '请输入 DeepSeek 登录密码。'
    # No shell eval/source and no secrets in argv, even for quotes/metacharacters.
    cr=$(printf '\015')
    case "$account$password$area" in *'
'*|*"$cr"*) fail '登录信息不能包含换行。';; esac
    put password "$password"; unset password
    ds_device
    case "$account" in
        *@*)
            case "$account" in *' '*|*'"'*|*'\\'*) fail '邮箱格式不正确。';; esac
            put email "$account"
            "$TOOL" object email "$WORK/email" password "$WORK/password" device_id "$WORK/device" os "$WORK/os" > "$WORK/form";;
        *)
            case "$area" in +*) digits=${area#+};; *) fail '手机号区号需以 + 开头，例如 +86。';; esac
            case "$digits" in ''|*[!0-9]*|????*) fail '手机号区号不正确。';; esac
            case "$account" in "$area"*) account=${account#"$area"};; esac
            case "$account" in ''|*[!0-9]*) fail '手机号只填写数字，国际区号在下方填写。';; esac
            [ "${#account}" -ge 5 ] && [ "${#account}" -le 15 ] || fail '手机号长度不正确。'
            put mobile "$account";put area "$area"
            "$TOOL" object mobile "$WORK/mobile" area_code "$WORK/area" password "$WORK/password" device_id "$WORK/device" os "$WORK/os" > "$WORK/form";;
    esac
    rm -f "$WORK/request" "$WORK/values" "$WORK/password"
    public '正在由音箱直连 DeepSeek 登录并检查账户身份…'
    ds_post /api/v0/users/login "$WORK/login"
    ds_finish_login 0
}

ds_finish_login() {
    ds_stage=login_user
    ds_user "$WORK/login" "$1" || ds_http_error "$WORK/login"
    access=$(get token "$WORK/user" 2>/dev/null || :)
    ds_verify "$subject"
    ds_save
    ds_account_notice
    if [ -n "$DS_NOTICE" ];then ui_settings configure "登录成功，但$DS_NOTICE";return;fi
    ui_settings configure 'DeepSeek 登录成功，账户身份检查通过。密码和短信验证码均未保存。'
}

account_status() {
    [ -f "$RECORD" ] || { ui_public login '尚未登录。请填写邮箱或手机号，选择密码或短信验证码登录。';return; }
    access=$(get access_token "$RECORD") || fail '本地账户记录损坏，请重新登录。'
    saved_subject=$(get subject "$RECORD") || fail '本地账户记录损坏，请重新登录。'
    ds_verify "$saved_subject"
    ds_save
    ds_account_notice
    if [ -n "$DS_NOTICE" ];then ui_public configure "登录凭据有效，但$DS_NOTICE";return;fi
    ui_public configure 'DeepSeek 登录状态有效，账户身份检查通过；尚未验证模型对话。'
}

dispatch() {
    prepare
    case "$1" in
        login) login;;
        send_sms) send_sms;;
        sms_login) sms_login;;
        status) account_status;;
        load)
            # The extension requires a public values object before enabling
            # actions. Return only empty account/default area, never secrets.
            if [ -f "$RECORD" ];then
                printf '%s\n' '{"step":"login","message":"音箱已保存 DeepSeek 凭据，点击检查登录验证是否仍有效。","values":{"account":"","area_code":"+86"}}'
            else
                printf '%s\n' '{"step":"login","message":"手机登录：填写已注册手机号，发送验证码后点击验证码登录；也可使用密码登录。此版只测试登录。","values":{"account":"","area_code":"+86"}}'
            fi;;
        website)
            put message '请打开下方官网链接，用已注册手机号获取短信验证码，再回到此窗口填写手机号和验证码，点击“验证码登录”。官网登录不会自动登录音箱。'
            put link https://chat.deepseek.com/sign_in
            put linkLabel '打开 DeepSeek 官网'
            "$TOOL" object message "$WORK/message" link "$WORK/link" linkLabel "$WORK/linkLabel";;
        logout) logout;;
        *) fail '未知操作。';;
    esac
}

case "${1:-}" in
    --web-action) dispatch "${2:-}";;
    --uninstall)
        "$SETTINGS" forget "$0" || exit 2
        ACCOUNT=$ROOT/deepseek
        [ ! -L "$ACCOUNT" ] || exit 2
        if [ -d "$ACCOUNT" ];then
            [ ! -L "$ACCOUNT/lock" ] || exit 2
            ( flock -n 9 || exit 2
              rm -f "$ACCOUNT/credentials.json" "$ACCOUNT/device-id" "$ACCOUNT"/.device.* "$ACCOUNT"/.credentials.* "$ACCOUNT/lock"
              rmdir "$ACCOUNT"
            ) 9>"$ACCOUNT/lock" || exit 2
        fi
        [ -L "$SMS_COOLDOWN" ] || rm -f "$SMS_COOLDOWN"
        [ -L "$DS_DIAGNOSTIC" ] || rm -f "$DS_DIAGNOSTIC"
        public 'DeepSeek 测试脚本的本地账户已清理，其他脚本未改变。';;
    '') public 'DeepSeek 登录测试脚本就绪，请打开扩展。此版不常驻、不接入唤醒。';;
    *) exit 2;;
esac
