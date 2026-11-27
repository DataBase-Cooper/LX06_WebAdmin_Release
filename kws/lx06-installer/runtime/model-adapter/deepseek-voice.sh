# Account inference protocol is replaceable with the uploaded script. Existing
# model-voice owns mic/ASR, rolling three-pair context, interruption and TTS.
# Build-session and PoW acquisition do not depend on each other. Keep each
# request's form/status/body in its own private directory and let the parent
# explicitly own both curl PIDs. No detached refresh or shared-response race.
# Kernel monotonic centiseconds, read in this shell without date/awk processes.
# Diagnostics contain fixed stage names and numeric durations only.
ds_trace_clock() {
    DS_CLOCK_MS=0
    if read -r ds_uptime ds_unused < /proc/uptime; then
        ds_seconds=${ds_uptime%.*};ds_fraction=${ds_uptime#*.}
        case "$ds_seconds" in ''|*[!0-9]*) return 0;; esac
        case "$ds_fraction" in [0-9][0-9]) DS_CLOCK_MS=$((ds_seconds * 1000 + (1$ds_fraction - 100) * 10));; esac
    fi
}
ds_trace_value() {
    ds_elapsed=$2;ds_total=$((DS_CLOCK_MS - DS_REQUEST_STARTED))
    [ "$ds_elapsed" -ge 0 ] || ds_elapsed=0
    [ "$ds_total" -ge 0 ] || ds_total=0
    printf 'deepseek-timing: stage=%s elapsed_ms=%s total_ms=%s\n' "$1" "$ds_elapsed" "$ds_total" >&2
}
ds_trace_span() {
    ds_trace_clock
    ds_trace_value "$1" "$((DS_CLOCK_MS - $2))"
}
ds_trace_fetch() {
    # Use each curl's own completion duration. Timing after wait(pow) would
    # overcount when it finished first. Keep the directly owned exec curl PID.
    ds_fetch_ms=0;ds_fetch_seconds=${ds_fetch_time%.*};ds_fetch_fraction=${ds_fetch_time#*.}
    case "$ds_fetch_seconds" in ''|*[!0-9]*) :;; *)
        case "$ds_fetch_fraction" in [0-9][0-9][0-9][0-9][0-9][0-9])
            ds_fetch_fraction=${ds_fetch_fraction%???}
            ds_fetch_ms=$((ds_fetch_seconds * 1000 + 1$ds_fetch_fraction - 1000));;
        esac;;
    esac
    ds_trace_clock
    ds_trace_value "$1" "$ds_fetch_ms"
}

ds_chat_start_request() {
    task_dir=$1;task_route=$2
    ( ulimit -f 2048
      exec curl --silent --show-error --proto '=https' --connect-timeout 12 --max-time 40 --max-filesize 1048576 --config "$WORK/auth" --data-binary "@$task_dir/form" --output "$task_dir/response" --write-out '%{http_code}\n%{time_total}\n' "$BASE_URL$task_route"
    ) > "$task_dir/status" 2>/dev/null &
    case "$3" in session) DS_SESSION_PID=$!;; pow) DS_POW_PID=$!;; esac
}

ds_chat_read_response() {
    task_dir=$1;target=$2;ds_stage=chat
    status=;ds_fetch_time=
    { IFS= read -r status || :; IFS= read -r ds_fetch_time || :; } < "$task_dir/status" 2>/dev/null || :
    case "$target" in /api/v0/chat_session/create) ds_trace_fetch session_fetch;; /api/v0/chat/create_pow_challenge) ds_trace_fetch pow_fetch;; esac
    [ "$DS_HTTP_OK" = 1 ] || ds_http_error "$task_dir/response"
    case "$status" in 2??) :;; *) ds_http_error "$task_dir/response";; esac
    [ "$(get code "$task_dir/response" 2>/dev/null || :)" = 0 ] || ds_http_error "$task_dir/response"
    get data "$task_dir/response" > "$task_dir/data" || ds_http_error "$task_dir/response"
    [ "$(get biz_code "$task_dir/data" 2>/dev/null || :)" = 0 ] || ds_http_error "$task_dir/response"
    get biz_data "$task_dir/data" > "$WORK/chat-business" || ds_http_error "$task_dir/response"
}

ds_chat_stop_requests() {
    # Clear PIDs immediately after wait so a later cleanup never signals a
    # completed/reaped request. Children exec curl directly, not another shell.
    if [ -n "${DS_SESSION_PID:-}" ];then
        kill -TERM "$DS_SESSION_PID" 2>/dev/null || :
        wait "$DS_SESSION_PID" 2>/dev/null || :
        DS_SESSION_PID=
    fi
    if [ -n "${DS_POW_PID:-}" ];then
        kill -TERM "$DS_POW_PID" 2>/dev/null || :
        wait "$DS_POW_PID" 2>/dev/null || :
        DS_POW_PID=
    fi
}

ds_chat_auth() {
    [ -f "$RECORD" ] || fail '请先在 DeepSeek 扩展中登录。'
    access=$(get access_token "$RECORD") || fail 'DeepSeek 账户记录无效，请重新登录。'
    ds_auth
    # Do not add a users/current round trip to every utterance. Actual chat
    # enforces token validity; a rejected token is never silently re-logged in.
}

ds_chat_cleanup() {
    # Stop owned network children before deleting any private request files.
    ds_chat_stop_requests
    # Delete only the transient session this request created. Never delete
    # existing website history. Best effort, no retry, bounded to five seconds.
    if [ -n "${DS_CHAT_ID:-}" ] && [ -n "$WORK" ] && [ -f "$WORK/auth" ];then
        ds_trace_clock;DS_DELETE_STARTED=$DS_CLOCK_MS
        put session-id "$DS_CHAT_ID"
        "$TOOL" object chat_session_id "$WORK/session-id" > "$WORK/delete-body"
        ( curl --silent --proto '=https' --connect-timeout 2 --max-time 5 --max-filesize 32768 --config "$WORK/auth" --data-binary "@$WORK/delete-body" --output "$WORK/delete-response" "$BASE_URL/api/v0/chat_session/delete" >/dev/null 2>&1 ) || :
        ds_trace_span delete_cleanup "$DS_DELETE_STARTED"
    fi
    cleanup
}

ds_infer() {
    ds_trace_clock;DS_REQUEST_STARTED=$DS_CLOCK_MS
    ds_chat_auth
    ds_trace_span auth "$DS_REQUEST_STARTED"
    DS_STAGE_STARTED=$DS_CLOCK_MS;ds_streamer
    ds_trace_span stream_helper "$DS_STAGE_STARTED"
    DS_STAGE_STARTED=$DS_CLOCK_MS
    if [ "${DS_STRUCTURED:-0}" != 1 ]; then
        "$WORK/deepseek-stream" --prompt < "$WORK/request" > "$WORK/prompt" || fail 'DeepSeek 对话上下文格式无效。'
    fi
    ds_trace_span prompt_build "$DS_STAGE_STARTED"
    DS_CHAT_ID=;DS_SESSION_PID=;DS_POW_PID=
    trap ds_chat_cleanup EXIT
    mkdir "$WORK/create-session" "$WORK/create-pow"
    printf '{}' > "$WORK/create-session/form"
    put completion-path /api/v0/chat/completion
    "$TOOL" object target_path "$WORK/completion-path" > "$WORK/create-pow/form"
    ds_chat_start_request "$WORK/create-session" /api/v0/chat_session/create session
    ds_chat_start_request "$WORK/create-pow" /api/v0/chat/create_pow_challenge pow
    DS_HTTP_OK=0;wait "$DS_SESSION_PID" && DS_HTTP_OK=1;DS_SESSION_PID=
    ds_chat_read_response "$WORK/create-session" /api/v0/chat_session/create
    get chat_session "$WORK/chat-business" > "$WORK/chat-session" || fail 'DeepSeek 未返回对话会话。'
    DS_CHAT_ID=$(get id "$WORK/chat-session" 2>/dev/null || :)
    case "$DS_CHAT_ID" in ''|*[!a-zA-Z0-9_-]*) DS_CHAT_ID=;fail 'DeepSeek 会话标识格式已变化。';; esac
    [ "${#DS_CHAT_ID}" -le 128 ] || { DS_CHAT_ID=;fail 'DeepSeek 会话标识过长。'; }
    DS_HTTP_OK=0;wait "$DS_POW_PID" && DS_HTTP_OK=1;DS_POW_PID=
    ds_chat_read_response "$WORK/create-pow" /api/v0/chat/create_pow_challenge
    get challenge "$WORK/chat-business" > "$WORK/puzzle" || fail 'DeepSeek 未返回对话校验。'
    ds_trace_clock;DS_STAGE_STARTED=$DS_CLOCK_MS
    ds_solve
    ds_trace_span pow_solve "$DS_STAGE_STARTED"
    DS_STAGE_STARTED=$DS_CLOCK_MS
    for key in algorithm challenge salt signature target_path;do
        get "$key" "$WORK/puzzle" > "$WORK/pow-$key" || fail 'DeepSeek 对话校验字段缺失。'
    done
    proof_path=$(cat "$WORK/pow-target_path")
    case "$proof_path" in /api/v0/chat/completion|/v0/chat/completion) :;; *) fail 'DeepSeek 对话校验目标不匹配。';; esac
    "$TOOL" object algorithm "$WORK/pow-algorithm" challenge "$WORK/pow-challenge" salt "$WORK/pow-salt" signature "$WORK/pow-signature" target_path "$WORK/pow-target_path" > "$WORK/pow-fields"
    sed 's/}$/,\"answer\":'"$answer"'}/' "$WORK/pow-fields" | "$WORK/auth-seal" --base64 > "$WORK/chat-proof"
    printf 'header = "x-ds-pow-response: %s"\n' "$(cat "$WORK/chat-proof")" >> "$WORK/auth"
    printf 'header = "Accept: text/event-stream"\n' >> "$WORK/auth"
    put session-id "$DS_CHAT_ID";put model "${MODEL_ID:-default}"
    "$TOOL" object chat_session_id "$WORK/session-id" model_type "$WORK/model" prompt "$WORK/prompt" > "$WORK/chat-fields"
    thinking=false;[ "${DS_THINKING:-0}" != 1 ] || thinking=true
    sed 's/}$/,\"parent_message_id\":null,\"ref_file_ids\":[],\"thinking_enabled\":'"$thinking"',\"search_enabled\":false}/' "$WORK/chat-fields" > "$WORK/form"
    if [ "${DS_STRUCTURED:-0}" = 1 ]; then structured_limit "$WORK/form" 8192;fi
    rm -f "$WORK/request" "$WORK/values"
    mkfifo "$WORK/stream"
    exec 6<>"$WORK/stream"
    "$WORK/deepseek-stream" "${DS_STREAM_MODE:---stream}" --error-file "$WORK/stream-error" < "$WORK/stream" 6>&- &
    STREAM_PID=$!
    transport_ok=0
    ds_trace_span request_build "$DS_STAGE_STARTED"
    ds_trace_span completion_start "$DS_REQUEST_STARTED"
    DS_COMPLETION_STARTED=$DS_CLOCK_MS
    http POST "$BASE_URL/api/v0/chat/completion" "$WORK/stream" "$WORK/auth" streaming && transport_ok=1
    ds_trace_span completion_transport "$DS_COMPLETION_STARTED"
    exec 6>&-
    parser_ok=0;wait "$STREAM_PID" && parser_ok=1;STREAM_PID=
    if [ -s "$WORK/stream-error" ];then
        target=/api/v0/chat/completion;ds_stage=completion_response
        ds_http_error "$WORK/stream-error"
    fi
    [ "$transport_ok" = 1 ] || request_failed
    [ "$parser_ok" = 1 ] || ds_failure 'DeepSeek 流式回复未完整结束或格式变化，已停止后续播报。'
    [ "${DS_STREAM_MODE:---stream}" != --stream ] || printf '%s\n' '{"done":true}'
}

model_request() { DS_VOICE_REQUEST=1;prepare;ds_infer; }
ds_text_test() {
    prepare
    question=$(field question)
    [ -n "$question" ] && [ "${#question}" -le 1800 ] || fail '请输入不超过 1800 字节的测试问题。'
    put question "$question";put role user
    "$TOOL" object role "$WORK/role" content "$WORK/question" > "$WORK/user-input"
    { printf '{"input":[';cat "$WORK/user-input";printf ']}'; } > "$WORK/request"
    DS_STREAM_MODE=--preview
    ds_infer
}

structured_account_ready() {
    [ -f "$RECORD" ] || fail '账户尚未登录，无法执行语义理解。'
    # The original verified status path preserves mute/identity diagnostics.
    account_status > "$WORK/structured-status-check"
    [ -z "${DS_NOTICE:-}" ] || fail 'DeepSeek 账户当前不可用于语义理解。'
}
structured_text_infer() {
    { cat "$WORK/structured-system";printf '\nRequest JSON:\n';cat "$WORK/structured-request"; } > "$WORK/prompt"
    structured_limit "$WORK/prompt" 8192
    DS_STRUCTURED=1;DS_STREAM_MODE=--collect
    ds_infer > "$WORK/structured-answer"
}
case "${1:-}" in
    --web-action)
        case "${2:-}" in
            status) structured_status;;
            extract|understand) structured_request "$2";;
            load)
                if [ -s "$ROOT/$PROVIDER/credentials.json" ]; then
                    ui_settings configure '已读取本机账户和对话设置。'
                else ui_settings login; fi;;
            save) account_save;;
            regenerate-greeting) account_greeting_action;;
            test) ds_text_test;;
            *) dispatch "${2:-}";;
        esac;;
    --model-request) model_request;;
    --session-worker) shift;voice_worker "$@";;
    --wakeup) [ "$#" -le 3 ] || exit 2; voice_resident --wakeup "${2:-}" "${3:-}";;
    --uninstall)
        deepseek_uninstall
        rm -f "/tmp/lx06-$PROVIDER-resident.lock" "/tmp/lx06-wake-${0##*/}.event"
        for lock in /tmp/lx06-mic/deepseek-*.lock;do
            [ -f "$lock" ] && [ ! -L "$lock" ] || continue
            ( flock -n 9 && rm -f "$lock" ) 9<"$lock" || :
        done;;
    '') voice_resident;;
    *) exit 2;;
esac
