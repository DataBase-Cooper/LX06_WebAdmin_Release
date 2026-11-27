# Included independently in each upload. Login and voice share only that
# provider's private credential directory; no dependency on another script.
voice_wait_init() {
    # This upload and its unified firmware are a matched release. The helper
    # only waits for mailbox changes: it never opens audio, reads accounts or
    # owns a conversation. Do not restore a per-100ms shell/sleep fallback.
    VOICE_WAIT_HELPER=${LX06_ACCOUNT_WAIT:-/usr/lib/lx06/account-wait}
    if [ -x "$VOICE_WAIT_HELPER" ] &&
       [ "$("$VOICE_WAIT_HELPER" --wait-protocol 2>/dev/null)" = LX06WAIT/1 ]; then return 0; fi
    printf '%s\n' 'account-wait: matched firmware idle-wait component missing; resident stopped' >&2
    return 1
}
voice_wait_event() {
    # A foreground external command can defer a shell TERM trap until it
    # returns. Track one background child and use the shell's builtin wait,
    # so keeper stop/uninstall immediately reaches resident_cleanup.
    # Only the resident owns its lock; the waiter must not retain descriptor 8.
    "$VOICE_WAIT_HELPER" --wait-event "$EVENT" 8>&- &
    WAIT_PID=$!
    _voice_wait_status=0
    wait "$WAIT_PID" || _voice_wait_status=$?
    WAIT_PID=
    [ "$_voice_wait_status" = 0 ] && return 0
    printf 'account-wait: idle-wait failed (%s); resident stopped\n' "$_voice_wait_status" >&2
    return 1
}
voice_owns_child() {
    while IFS='	 ' read -r field value rest; do
        if [ "$field" = PPid: ]; then
            [ "$value" = "$$" ]
            return $?
        fi
    done 2>/dev/null <"/proc/$1/status"
    return 1
}
voice_reap_child() {
    [ -n "$1" ] || return 0
    if voice_owns_child "$1"; then kill -TERM "$1" 2>/dev/null || :; fi
    _reap_attempt=0
    while voice_owns_child "$1" && [ "$_reap_attempt" -lt 12 ]; do
        sleep .05
        _reap_attempt=$((_reap_attempt+1))
    done
    if voice_owns_child "$1"; then kill -KILL "$1" 2>/dev/null || :; fi
    wait "$1" 2>/dev/null || :
}
voice_http_start() {
    unset LX06_MODEL_HTTP_SOCKET LX06_MODEL_HTTP_HELPER LX06_MODEL_HTTP_READY LX06_MODEL_HTTP_PID
    [ -x "${HTTP:-}" ] || return 0
    HTTP_WARM_URL=
    # This provider hook only assigns a public same-origin URL. Anonymous
    # HEAD opens DNS/TCP/TLS without reading credentials, taking account locks,
    # creating chat history, submitting a question or consuming model tokens.
    if awk '/^MemAvailable:/ { available=$2 } /^MemFree:/ { physical=$2 }
            END { exit !(available >= 32768 && physical >= 10240) }' /proc/meminfo &&
       command -v provider_preconnect >/dev/null 2>&1 && provider_preconnect &&
       [ -n "$HTTP_WARM_URL" ]; then
        # Optional preparation errors (for example full TMP) must preserve the
        # original greeting/voice path. Nothing was handed to a broker yet.
        if ! { mkdir "$SESSION/http-warm" &&
               printf '%s' HEAD > "$SESSION/http-warm/method" &&
               printf '%s' "$HTTP_WARM_URL" > "$SESSION/http-warm/url" &&
               : > "$SESSION/http-warm/headers" &&
               : > "$SESSION/http-warm/body" &&
               printf '%s' 6 > "$SESSION/http-warm/timeout"; }; then return 0; fi
        "$HTTP" --serve "$SESSION/http.sock" --warm "$SESSION/http-warm" >/dev/null &
        HTTP_PID=$!
        export LX06_MODEL_HTTP_READY="$SESSION/http-warm/ready"
    else
        "$HTTP" --serve "$SESSION/http.sock" >/dev/null &
        HTTP_PID=$!
    fi
    export LX06_MODEL_HTTP_PID="$HTTP_PID"
    export LX06_MODEL_HTTP_SOCKET="$SESSION/http.sock"
    export LX06_MODEL_HTTP_HELPER="$HTTP"
}
voice_worker() {
    umask 077
    DIALOGUED=${LX06_DIALOGUED:-/usr/bin/lx06-dialogued}
    [ -x "$DIALOGUED" ] && [ "$("$DIALOGUED" --protocol 2>/dev/null)" = LX06DIALOGUE/1 ] || {
        echo '固件缺少统一会话组件，请更新固件。'; return 1;
    }
    SESSION=$(mktemp -d /tmp/lx06-account-mic.XXXXXX) || return 1
    CORE=; HTTP_PID=
    voice_cleanup() {
        cleanup_status=$?
        trap - EXIT
        trap '' TERM INT HUP
        voice_reap_child "$CORE"
        voice_reap_child "$HTTP_PID"
        cleanup_incomplete=0
        if [ -d "$SESSION/http-warm" ]; then
            rm -f "$SESSION/http-warm/method" "$SESSION/http-warm/url" "$SESSION/http-warm/headers" "$SESSION/http-warm/body" "$SESSION/http-warm/timeout" "$SESSION/http-warm/ready" 2>/dev/null || cleanup_incomplete=1
            rmdir "$SESSION/http-warm" 2>/dev/null || cleanup_incomplete=1
        fi
        rm -f "$SESSION/http.sock" 2>/dev/null || cleanup_incomplete=1
        rmdir "$SESSION" 2>/dev/null || cleanup_incomplete=1
        if [ "$cleanup_incomplete" != 0 ]; then
            printf '%s\n' 'account-voice: session cleanup incomplete' >&2
            [ "$cleanup_status" != 0 ] || cleanup_status=1
        fi
        exit "$cleanup_status"
    }
    trap voice_cleanup EXIT
    trap 'exit 0' TERM INT HUP
    # Scripts own the provider's HTTP pool and credentials only. Microphone,
    # ASR, playback, interruption and lights belong to the one firmware core.
    # The public anonymous HEAD starts at wake, before the first question.
    voice_http_start
    DC_MAX_SECONDS="${MODEL_MAX_SECONDS:-900}" "$DIALOGUED" --account "$0" "$MODEL_ID" "${1:-}" &
    CORE=$!
    core_status=0
    wait "$CORE" 2>/dev/null || core_status=$?
    CORE=
    # Preserve every child status; a numeric status alone does not identify its cause.
    return "$core_status"
}
voice_warmup() {
    # The unified core owns greeting synthesis. Do not start the retired audio
    # controller in parallel at resident startup or after a conversation.
    return 0
}
account_greeting_action() {
    "$SETTINGS" greeting "${LX06_SCRIPT_PATH:-$0}"
}
account_save() {
    "$SETTINGS" save "$0" || return $?
}
voice_session() {
    WAKEWORD=${1:-}
    _voice_ticket=${2:-}
    _voice_epoch=
    _voice_expires=
    case "$_voice_ticket" in
        *:*) _voice_epoch=${_voice_ticket%%:*}; _voice_expires=${_voice_ticket#*:};;
        '') ;;
        *) return 2;;
    esac
    [ -z "$_voice_ticket" ] || { [ -n "$_voice_epoch" ] && [ -n "$_voice_expires" ]; } || return 2
    # Give the actual conversation priority over our optional idle warmer.
    if [ -n "${WARMUP_PID:-}" ]; then
        kill -TERM "$WARMUP_PID" 2>/dev/null || :
        wait "$WARMUP_PID" 2>/dev/null || :
        WARMUP_PID=
    fi
    DIALOGUED=${LX06_DIALOGUED:-/usr/bin/lx06-dialogued}
    [ -x "$DIALOGUED" ] && [ -x "$SETTINGS" ] || { echo '统一会话组件未部署，请更新固件。'; return 1; }
    "$SETTINGS" run "$0" sh -c '
        export LX06_DIALOG_LIGHTS=1 LX06_SESSION_MAX_SECONDS="$MODEL_MAX_SECONDS"
        if [ -n "$5" ]; then
            exec "$1/session-run" run-local "$3" "$5" "$6" sh "$2" --session-worker "$4"
        fi
        exec "$1/session-run" run "$3" sh "$2" --session-worker "$4"
    ' lx06 "$XA" "$0" "${0##*/}" "$WAKEWORD" "$_voice_epoch" "$_voice_expires" &
    SESSION_PID=$!
    wait "$SESSION_PID" || :
    SESSION_PID=
    voice_warmup
}
# Explicit phone caller shared by sanitized phone uploads. It never starts an
# account HTTP pool, reads credentials, or infers a wake source from --wakeup.
phone_session() {
    [ "$#" = 2 ] || return 2
    case "$1" in official_asr|local_wake) ;; *) return 2;; esac
    [ "${LX06_SCRIPT_EXEC_PROTOCOL:-}" = shell-fd-v1 ] &&
    [ -n "${LX06_SCRIPT_PATH:-}" ] && [ -n "${LX06_SCRIPT_ID:-}" ] &&
    [ "${LX06_SCRIPT_PATH##*/}" = "$LX06_SCRIPT_ID" ] || return 2
    DIALOGUED=${LX06_DIALOGUED:-/usr/bin/lx06-dialogued}
    [ -x "$DIALOGUED" ] && [ -x "$XA/session-run" ] || return 1
    LX06_DIALOG_LIGHTS=1 LX06_SESSION_MAX_SECONDS="${MODEL_MAX_SECONDS:-900}" \
        "$XA/session-run" run "$LX06_SCRIPT_ID" --phone-session "$1" "$2" &
    SESSION_PID=$!
    _phone_status=0
    wait "$SESSION_PID" || _phone_status=$?
    SESSION_PID=
    return "$_phone_status"
}
voice_resident() {
    _voice_phone=0
    _voice_id=${0##*/}
    if [ "${1:-}" = --phone-wakeup ]; then
        _voice_phone=1
        [ "${LX06_SCRIPT_EXEC_PROTOCOL:-}" = shell-fd-v1 ] &&
        [ -n "${LX06_SCRIPT_ID:-}" ] || return 2
        _voice_id=$LX06_SCRIPT_ID
    fi
    EVENT=/tmp/lx06-wake-$_voice_id.event
    SESSION_PID=
    WARMUP_PID=
    WAIT_PID=
    resident_cleanup() {
        cleanup_status=$?
        trap - EXIT
        trap '' TERM INT HUP
        voice_reap_child "$WAIT_PID"
        # The session owner provides its own group kill and reap, including
        # descended capture/transport processes. Do not kill the owner first.
        if [ -n "$SESSION_PID" ]; then
            if voice_owns_child "$SESSION_PID"; then kill -TERM "$SESSION_PID" 2>/dev/null || :; fi
            wait "$SESSION_PID" 2>/dev/null || :
        fi
        voice_reap_child "$WARMUP_PID"
        if ! rm -f "$EVENT" 2>/dev/null; then
            printf '%s\n' 'account-voice: resident event cleanup incomplete' >&2
            [ "$cleanup_status" != 0 ] || cleanup_status=1
        fi
        exit "$cleanup_status"
    }
    exec 8>"/tmp/lx06-$PROVIDER-resident.lock"
    flock -n 8 || return 0
    trap resident_cleanup EXIT
    trap 'exit 0' TERM INT HUP
    rm -f "$EVENT"
    voice_wait_init || return 1
    # A direct wake gets the session first; no warmup competes with its first
    # greeting. Ordinary resident startup is already idle and may prewarm.
    if [ "$_voice_phone" = 1 ]; then
        phone_session "${2:-}" "${3:-}" || return $?
    elif [ "${1:-}" = --wakeup ]; then voice_session "${2:-}" "${3:-}" || return $?; else voice_warmup; fi
    echo '账户语音入口就绪；等待本地唤醒或小爱通道唤醒。'
    while :; do
        if [ -n "$WARMUP_PID" ] && ! kill -0 "$WARMUP_PID" 2>/dev/null; then wait "$WARMUP_PID" || :; WARMUP_PID=; fi
        if [ -s "$EVENT" ]; then
            source=; _phone_extra=; _voice_ticket=; _voice_extra=
            if [ "$_voice_phone" = 1 ]; then
                IFS='	' read -r word score stamp source _phone_extra < "$EVENT" || :
            else IFS=' ' read -r word score stamp _voice_ticket _voice_extra < "$EVENT" || :; fi
            rm -f "$EVENT"
            read -r uptime rest < /proc/uptime
            case "$stamp" in ''|*[!0-9.]*) stamp=0;; esac
            if [ -n "$word" ] && [ "${uptime%%.*}" -ge "${stamp%%.*}" ] &&
               [ "$(( ${uptime%%.*} - ${stamp%%.*} ))" -le 3 ]; then
                if [ "$_voice_phone" = 1 ]; then
                    [ -n "$source" ] && [ -z "$_phone_extra" ] && phone_session "$source" "$word"
                elif [ -z "$_voice_extra" ]; then voice_session "$word" "$_voice_ticket"; fi
            fi
        fi
        voice_wait_event || return $?
    done
}
# Audio components come from the unified firmware. Never unpack or start the
# retired per-provider audio controller as a fallback during wake/session entry.
# Explicit greeting-cache actions below remain separately documented migration
# work until the core exposes the cache-generation API.
case "${1:-}" in
    --web-action)
        case "${2:-}" in status) structured_status;; extract|understand) structured_request "$2";; load) account_load;; save) account_save;; regenerate-greeting) account_greeting_action;; *) dispatch "${2:-}";; esac;;
    --model-request) model_request;;
    --session-worker) shift; voice_worker "$@";;
    --wakeup) [ "$#" -le 3 ] || exit 2; voice_resident --wakeup "${2:-}" "${3:-}";;
    --phone-wakeup) [ "$#" = 3 ] || exit 2; voice_resident --phone-wakeup "$2" "$3";;
    --uninstall)
        if command -v provider_uninstall >/dev/null 2>&1; then provider_uninstall; fi
        "$SETTINGS" forget "$0" || exit 2
        # Web/keeper first stop this resident and its extension process tree.
        # Remove only this provider's credentials; never another account.
        case "$PROVIDER" in grok|chatgpt) ;; *) exit 2;; esac
        account="$ROOT/$PROVIDER"
        if [ -d "$account" ] && [ ! -L "$account" ]; then
            [ ! -L "$account/lock" ] || exit 2
            ( flock -n 9 || exit 2
              rm -f "$account/credentials.json" "$account"/.credentials.* "$account/lock"
              rmdir "$account"
            ) 9>"$account/lock" || exit 2
        fi
        rm -f "/tmp/lx06-$PROVIDER-resident.lock" "/tmp/lx06-wake-${0##*/}.event"
        for lock in /tmp/lx06-mic/"$PROVIDER"-*.lock; do
            [ -f "$lock" ] && [ ! -L "$lock" ] || continue
            ( flock -n 9 && rm -f "$lock" ) 9<"$lock" || :
        done
        public '已卸载并清理本地账户。';;
    '') voice_resident;;
    *) exit 2;;
esac
