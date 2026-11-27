# Included at build time: each upload is self-contained. No runtime sourcing
# of another user's script. Caller supplies PROVIDER/TOKEN_URL/BASE_URL.
set -eu
umask 077
TOOL=/usr/bin/lx06-model-tool
ROOT=/data/lx06/model-accounts
WORK=
CALLBACK_PID=
STREAM_PID=
pending=
cleanup() {
    if [ -n "$STREAM_PID" ]; then kill "$STREAM_PID" 2>/dev/null || :; wait "$STREAM_PID" 2>/dev/null || :; fi
    if [ -n "$CALLBACK_PID" ]; then kill "$CALLBACK_PID" 2>/dev/null || :; wait "$CALLBACK_PID" 2>/dev/null || :; fi
    [ -z "$WORK" ] || rm -rf "$WORK"
    [ -z "$pending" ] || rm -f "$pending"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
public() { printf '%s' "$1" | "$TOOL" quote | { IFS= read -r line; printf '{"message":%s}\n' "$line"; }; }
ui_public() {
    put ui_step "$1"; put message "$2"
    "$TOOL" object step "$WORK/ui_step" message "$WORK/message"
}
ui_settings() {
    # show emits exactly one already validated, password-free JSON object.
    # Prefix a script-owned step without reparsing or shell evaluation.
    ui_json=$("$SETTINGS" show "$0") || { printf '%s\n' "$ui_json"; return 1; }
    if [ -n "${2:-}" ]; then
        ui_values=$(printf '%s' "$ui_json" | "$TOOL" get values) || return 1
        ui_note=$(printf '%s' "$2" | "$TOOL" quote) || return 1
        printf '{"step":"%s","message":%s,"values":%s}\n' "$1" "$ui_note" "$ui_values"
        return
    fi
    printf '{"step":"%s",%s\n' "$1" "${ui_json#\{}"
}
fail() { public "$1"; exit 1; }
get() { "$TOOL" get "$1" < "$2"; }
put() { printf '%s' "$2" > "$WORK/$1"; }
field() { get "$1" "$WORK/values" 2>/dev/null || :; }
form() { "$TOOL" form "$@" > "$WORK/form"; }
model_http_ready() {
    [ -n "${LX06_MODEL_HTTP_READY:-}" ] || return 0
    # ready means bind+listen, not completed HEAD. Once ready, POST must use
    # the broker's backlog and wait for its prewarm; never create another TLS
    # connection just because warm is still running. No credential lock is
    # involved in anonymous warm preparation.
    broker_wait=0
    while [ "$broker_wait" -lt 40 ]; do
        [ -n "${LX06_MODEL_HTTP_PID:-}" ] && kill -0 "$LX06_MODEL_HTTP_PID" 2>/dev/null || return 1
        if [ -f "$LX06_MODEL_HTTP_READY" ] && [ ! -L "$LX06_MODEL_HTTP_READY" ] &&
           [ -S "${LX06_MODEL_HTTP_SOCKET:-}" ]; then return 0; fi
        broker_wait=$((broker_wait + 1));sleep .05
    done
    # A live but not-listening broker is a setup failure, not permission to
    # race it with curl. Session cleanup remains the sole owner of its PID.
    [ -n "${LX06_MODEL_HTTP_PID:-}" ] && kill -0 "$LX06_MODEL_HTTP_PID" 2>/dev/null || return 1
    return 2
}
# All responses live in private bounded TMP. No redirects, no insecure TLS,
# no tokens in argv. curl errors/raw provider responses never reach Web logs.
http() {
    method=$1; endpoint=$2; output=$3; config=${4:-$WORK/empty}
    budget=40; is_streaming=0
    if [ "${5:-}" = streaming ]; then set -- --no-buffer --fail; budget=120; is_streaming=1; else set --; fi
    broker_ready=1
    if [ "$is_streaming" = 1 ] && [ "$method" = POST ] && [ -x "${LX06_MODEL_HTTP_HELPER:-}" ]; then
        broker_ready=0;model_http_ready || broker_ready=$?
        [ "$broker_ready" != 2 ] || return 1
    fi
    # Only inference streams use the session-owned native connection pool.
    # Login, token renewal and parallel DeepSeek setup retain their existing
    # short-lived transport. The helper sees file paths, never token argv.
    if [ "$is_streaming" = 1 ] && [ "$method" = POST ] && [ "$broker_ready" = 0 ] &&
       [ -x "${LX06_MODEL_HTTP_HELPER:-}" ] && [ -S "${LX06_MODEL_HTTP_SOCKET:-}" ]; then
        broker_dir=$(mktemp -d "$WORK/http.XXXXXX") || return 1
        if ! { printf '%s' POST > "$broker_dir/method" &&
               printf '%s' "$endpoint" > "$broker_dir/url" &&
               cp "$config" "$broker_dir/headers" &&
               cp "$WORK/form" "$broker_dir/body" &&
               printf '%s' "$budget" > "$broker_dir/timeout"; }; then
            rm -f "$broker_dir/method" "$broker_dir/url" "$broker_dir/headers" "$broker_dir/body" "$broker_dir/timeout"
            rmdir "$broker_dir" 2>/dev/null || :
            return 1
        fi
        broker_rc=0
        "$LX06_MODEL_HTTP_HELPER" --request "$LX06_MODEL_HTTP_SOCKET" "$broker_dir" > "$output" 6>&- || broker_rc=$?
        status=$(cat "$broker_dir/status" 2>/dev/null || printf '%s' 000)
        rm -f "$broker_dir/method" "$broker_dir/url" "$broker_dir/headers" "$broker_dir/body" "$broker_dir/timeout" "$broker_dir/status"
        rmdir "$broker_dir" 2>/dev/null || :
        case "$broker_rc" in
            0) case "$status" in 2??) return 0;; *) return 1;; esac;;
            22) return 2;;
            75) :;; # Socket unavailable before sending: safe curl fallback.
            *) return 1;; # Never replay an already submitted model POST.
        esac
    fi
    if [ "$method" = POST ]; then
        status=$( (ulimit -f 2048; exec curl "$@" --silent --show-error --proto '=https' --connect-timeout 12 --max-time "$budget" --max-filesize 1048576 --config "$config" --data-binary "@$WORK/form" --output "$output" --write-out '%{http_code}' "$endpoint" 6>&-) 2>/dev/null) || return 1
    else
        status=$( (ulimit -f 2048; exec curl --silent --show-error --proto '=https' --connect-timeout 12 --max-time 40 --max-filesize 1048576 --config "$config" --output "$output" --write-out '%{http_code}' "$endpoint") 2>/dev/null) || return 1
    fi
    case "$status" in 2??) return 0;; *) return 2;; esac
}
token_safe() { case "$1" in ''|*[!a-zA-Z0-9._~+/=-]*) return 1;; esac; }
expiry_valid() { case "$1" in ''|*[!0-9]*|?????????*) return 1;; esac; [ "$1" -gt 0 ] && [ "$1" -le 31536000 ]; }
has_scope() { case " $1 " in *" $2 "*) return 0;; *) return 1;; esac; }
prepare() {
    [ -x "$TOOL" ] || { printf '%s\n' '{"message":"固件缺少通用账户工具，请使用候选测试环境。"}'; exit 1; }
    [ "$(id -u)" = 0 ] || fail '账户脚本需要受管 root 权限。'
    mkdir -p "$ROOT"
    [ ! -L "$ROOT" ] && [ -d "$ROOT" ] || fail '账户目录异常。'
    chmod 700 "$ROOT"
    ACCOUNT=$ROOT/$PROVIDER
    [ ! -L "$ACCOUNT" ] || fail '账户目录异常。'
    mkdir -p "$ACCOUNT"; chmod 700 "$ACCOUNT"
    [ ! -L "$ACCOUNT/lock" ] || fail '账户锁异常。'
    exec 9>"$ACCOUNT/lock"
    flock -n 9 || fail '账户正在执行另一项操作，请稍后再试。'
    WORK=$(mktemp -d /tmp/lx06-account.XXXXXX)
    : > "$WORK/empty"
    # The Web layer already enforces request bounds/types; repeat JSON parsing
    # here so direct shell execution cannot turn input into shell syntax.
    cat > "$WORK/request"
    get values "$WORK/request" > "$WORK/values" || fail '操作参数无效。'
    RECORD=$ACCOUNT/credentials.json
    [ ! -L "$RECORD" ] || fail '账户文件异常。'
}
save_tokens() {
    # Caller has validated identity on initial ChatGPT login. Refresh preserves
    # identity/client binding and atomically replaces the entire token record.
    access=$(get access_token "$WORK/token" 2>/dev/null) || fail '平台未返回访问凭据。'
    token_safe "$access" || fail '访问凭据格式无效。'
    kind=$(get token_type "$WORK/token" 2>/dev/null || :)
    [ "$kind" = Bearer ] || [ "$kind" = bearer ] || fail '平台返回不支持的凭据类型。'
    seconds=$(get expires_in "$WORK/token" 2>/dev/null || :)
    expiry_valid "$seconds" || fail '平台凭据有效期无效。'
    refresh=$(get refresh_token "$WORK/token" 2>/dev/null || :)
    if [ -z "$refresh" ] && [ "$SAVING_REFRESH" = 1 ]; then refresh=$(get refresh_token "$RECORD"); fi
    token_safe "$refresh" || fail '未取得可供音箱独立续期的凭据。'
    scope=$(get scope "$WORK/token" 2>/dev/null || :)
    if [ -z "$scope" ] && [ "$SAVING_REFRESH" = 1 ]; then scope=$(get scope "$RECORD"); fi
    has_scope "$scope" "$REQUIRED_SCOPE" || fail '账户没有授予对话权限，原凭据未覆盖。'
    put access "$access"; put refresh "$refresh"; put scope "$scope"
    put expiry "$(( $(date +%s) + seconds ))"
    idtoken=$(get id_token "$WORK/token" 2>/dev/null || :)
    if [ -z "$idtoken" ] && [ "$SAVING_REFRESH" = 1 ]; then idtoken=$(get id_token "$RECORD" 2>/dev/null || :); fi
    put idtoken "$idtoken"
    pending=$(mktemp "$ACCOUNT/.credentials.XXXXXX")
    if ! "$TOOL" object access_token "$WORK/access" refresh_token "$WORK/refresh" scope "$WORK/scope" expires_at "$WORK/expiry" client_id "$WORK/client" subject "$WORK/subject" id_token "$WORK/idtoken" > "$pending"; then rm -f "$pending"; fail '无法保存账户。'; fi
    chmod 600 "$pending"
    mv -f "$pending" "$RECORD"
    pending=
}
ensure_access() {
    [ -f "$RECORD" ] || fail '请先登录账户。'
    expiry=$(get expires_at "$RECORD")
    case "$expiry" in ''|*[!0-9]*) fail '账户记录损坏，请重新登录。';; esac
    if [ "$expiry" -le "$(( $(date +%s) + 90 ))" ]; then
        get client_id "$RECORD" > "$WORK/client"
        get subject "$RECORD" > "$WORK/subject"
        get refresh_token "$RECORD" > "$WORK/refresh"
        put grant refresh_token
        refresh_form
        http POST "$TOKEN_URL" "$WORK/token" || fail '账户续期失败；原凭据保留，请检查网络或重新登录。'
        SAVING_REFRESH=1; save_tokens
    fi
    access=$(get access_token "$RECORD")
    token_safe "$access" || fail '账户访问凭据无效。'
    printf 'header = "Authorization: Bearer %s"\n' "$access" > "$WORK/auth"
    provider_headers >> "$WORK/auth"
}
request_options() {
    # Optional, provider-owned Responses fields. Text tests and voice requests
    # share this hook; providers without it keep their existing wire format.
    if command -v provider_request_options >/dev/null 2>&1; then provider_request_options "$model"; fi
}
# The existing bounded text transport is also the structured adapter callback.
# Input is a JSON conversation in WORK/input; output is complete {text:...}.
account_text_infer() {
    ensure_access
    put model "$model"
    { printf '{"model":'; "$TOOL" quote < "$WORK/model"; request_options; printf ',"store":false,"stream":true,"input":';cat "$WORK/input";printf '}'; } > "$WORK/form"
    if [ -n "${ACCOUNT_TEXT_LIMIT:-}" ]; then
        [ "$(wc -c < "$WORK/form")" -le "$ACCOUNT_TEXT_LIMIT" ] || fail '结构化模型数据超出限制。'
    fi
    printf 'header = "Content-Type: application/json"\nheader = "Accept: text/event-stream"\n' >> "$WORK/auth"
    http POST "$BASE_URL/responses" "$WORK/sse" "$WORK/auth" || request_failed
    "$TOOL" sse type response.output_text.delta delta response.completed response.failed,response.incomplete,error < "$WORK/sse" > "$WORK/text-answer" || fail '平台未返回完整正文，未把断流当作成功。'
}
structured_account_ready() { ensure_access; }
structured_text_infer() {
    model=${MODEL_ID:-}
    [ -n "$model" ] || fail '请先选择并保存模型。'
    { printf '[{"role":"system","content":';"$TOOL" quote < "$WORK/structured-system"
      printf '},{"role":"user","content":';"$TOOL" quote < "$WORK/structured-request";printf '}]'
    } > "$WORK/input"
    ACCOUNT_TEXT_LIMIT=8192
    account_text_infer
    cp "$WORK/text-answer" "$WORK/structured-answer"
}
text_test() {
    model=$(field model); question=$(field question)
    [ -n "$model" ] || fail '请从模型列表选择模型。'
    [ -n "$question" ] || fail '请输入测试问题。'
    put question "$question"
    { printf '[{"role":"user","content":';"$TOOL" quote < "$WORK/question";printf '}]'; } > "$WORK/input"
    public '音箱正在直连平台进行文字测试…'
    account_text_infer
    get text "$WORK/text-answer" | "$TOOL" clip 1800 > "$WORK/public"
    "$TOOL" object message "$WORK/public"
}
model_request() {
    # C supplies a bounded conversation array, never shell code. Platform
    # response mapping and protocol fields remain replaceable in this script.
    prepare
    model=$(field model)
    [ -n "$model" ] || fail '请先在扩展中选择模型并保存。'
    get input "$WORK/request" > "$WORK/input" || fail '语音请求格式无效。'
    ensure_access
    put model "$model"
    { printf '{"model":'; "$TOOL" quote < "$WORK/model"; request_options; printf ',"store":false,"stream":true,"input":'; cat "$WORK/input"; printf '}'; } > "$WORK/form"
    printf 'header = "Content-Type: application/json"\nheader = "Accept: text/event-stream"\n' >> "$WORK/auth"
    # A FIFO bounds memory and forwards the first text without waiting for the
    # complete answer. FD6 holds the FIFO open until curl exits, including the
    # case where TLS/HTTP fails before curl ever opens its output file.
    mkfifo "$WORK/stream"
    exec 6<>"$WORK/stream"
    "$TOOL" sse-stream type response.output_text.delta delta response.completed response.failed,response.incomplete,error < "$WORK/stream" 6>&- &
    STREAM_PID=$!
    transport_ok=0
    http POST "$BASE_URL/responses" "$WORK/stream" "$WORK/auth" streaming && transport_ok=1
    exec 6>&-
    parser_ok=0
    wait "$STREAM_PID" && parser_ok=1
    STREAM_PID=
    [ "$transport_ok" = 1 ] || request_failed
    [ "$parser_ok" = 1 ] || fail '平台流未完整结束；已停止后续播报。'
    printf '%s\n' '{"done":true}'
}
request_failed() {
    # Status alone is safe to display; never expose a raw provider error body.
    case "${status:-}" in
        400|422) fail '平台拒绝请求格式，请更新适配脚本。';;
        401) fail '登录凭据未获平台认可，请重新登录。';;
        402|403) fail '当前账户尚无此模型的调用权限或可用额度。';;
        426) fail '平台要求更新协议客户端版本，请更新适配脚本，无需重新刷机。';;
        429) fail '平台限制请求频率或账户额度不足，请稍后再试。';;
        *) fail '平台请求失败，请检查网络或稍后重试。';;
    esac
}
models() {
    ensure_access
    http GET "$BASE_URL/models" "$WORK/models" "$WORK/auth" || fail '无法获取账户可用模型。'
    # Only public IDs/labels enter the dropdown. Parse the entire bounded
    # catalog before emitting one event; failed refreshes preserve the old
    # selection and menu. No inference request is sent by this action.
    set -- model
    if [ -n "${MODEL_LIST_SPECS:-}" ]; then
        for spec in $MODEL_LIST_SPECS; do
            IFS='|' read -r root id label filter value <<EOF
$spec
EOF
            [ -n "$root" ] && [ -n "$id" ] || continue
            set -- "$@" "$root" "$id" "$label" "${filter:--}" "${value:--}"
        done
    else
        set -- model "$MODEL_LIST_KEY" "$MODEL_ID_KEY" "$MODEL_LABEL_KEY"
        if [ -n "${MODEL_FILTER_KEY:-}" ]; then set -- "$@" "$MODEL_FILTER_KEY" "$MODEL_FILTER_VALUE"; fi
    fi
    "$TOOL" list-options "$@" < "$WORK/models" > "$WORK/model-options" || fail '模型列表超出限制或格式已变化，原选择保留，请更新脚本后重试。'
    cat "$WORK/model-options"
}
account_load() {
    # Read settings first; fetching a missing catalog must never replace a
    # previously saved model with a default, nor make opening the dialog send
    # a model inference. The runner commits options only after success.
    if command -v provider_ui_pending >/dev/null 2>&1 && provider_ui_pending available; then
        prepare; provider_ui_pending; return
    fi
    if [ ! -f "$ROOT/$PROVIDER/credentials.json" ]; then ui_settings login; return; fi
    prepare
    if ! (ensure_access); then ui_settings login '登录验证未通过，请重新登录或检查网络。原账户记录保留。'; return; fi
    # The runner retains this action's successful values across NDJSON events;
    # the options event need not duplicate the full form within its 8 KiB cap.
    if [ -n "${MODEL_LIST_KEY:-}" ] && ! "$SETTINGS" has-options "$0"; then
        (models) || public '配置已保留；暂未获取模型，请点击更新模型列表重试。'
    fi
    ui_settings configure
}
account_status() {
    if [ -f "$RECORD" ]; then
        ensure_access
        ui_public configure '登录凭据有效。剩余额度：平台未提供可验证的查询接口，请在官网查看。'
    else ui_public login '尚未登录。电脑或手机只协助授权，音箱独立对话。'; fi
}
account_forget_model_options() {
    # Public model permissions belong to the current login. The primitive is
    # generic; firmware does not know provider names or account endpoints.
    [ -z "${MODEL_LIST_KEY:-}" ] || "$SETTINGS" forget-options "$0" >/dev/null 2>&1 || :
}
logout() {
    # Local disconnect is deliberately explicit; do not claim provider-side
    # revocation without a documented and successful revocation operation.
    rm -f "$RECORD"
    account_forget_model_options
    ui_public login '已退出本地账户。撤销平台授权请到官网账户设置操作。'
}
dispatch() {
    prepare
    case "$1" in
        login)
            login
            if [ "${ACCOUNT_LOGIN_USING_CALLBACK:-0}" != 1 ]; then account_forget_model_options; fi
            if command -v provider_login_ready >/dev/null 2>&1; then provider_login_ready; fi;;
        complete-login)
            command -v complete_login >/dev/null 2>&1 || fail '未知操作。'
            complete_login
            if [ "${ACCOUNT_MODELS_COMMITTED:-0}" != 1 ]; then account_forget_model_options; fi;;
        login-auto)
            command -v login_automatic >/dev/null 2>&1 || fail '未知操作。'
            login_automatic
            if [ "${ACCOUNT_MODELS_COMMITTED:-0}" != 1 ]; then account_forget_model_options; fi;;
        status) account_status;; models) models;; test) text_test;; logout) logout;; *) fail '未知操作。';;
    esac
}
