#!/bin/sh
# Exercise the actual worker and EXIT trap in the invoking POSIX shell; no device.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEST_ROOT=$(mktemp -d "${TMPDIR:?set TMPDIR to scratch}/voice-cleanup.XXXXXX")
trap 'command rm -rf "$TEST_ROOT"' EXIT HUP INT TERM
export TEST_ROOT
awk '/^# Audio components come from the unified firmware\./ {exit} {print}' "$HERE/account-voice.sh" > "$TEST_ROOT/worker"
cat >> "$TEST_ROOT/worker" <<'WORKER'
mktemp() { [ "${FAIL_KIND:-}" != mktemp ] || return 1; printf '%s\n' "$TEST_ROOT/session"; }
voice_http_start() {
    mkdir "$SESSION/http-warm"
    for item in method url headers body timeout ready; do printf '%s' private > "$SESSION/http-warm/$item"; done
    printf '%s' socket > "$SESSION/http.sock"
    if [ "$FAIL_KIND" = unknown ]; then printf '%s' keep > "$SESSION/unrelated"; fi
}
rm() {
    case "$FAIL_KIND:$*" in
        warm:*http-warm/method*) return 1;;
        socket:*http.sock*) return 1;;
    esac
    command rm "$@"
}
MODEL_ID=fixture
LX06_DIALOGUED=$TEST_ROOT/core
voice_worker
WORKER
cat > "$TEST_ROOT/core" <<'CORE'
#!/bin/sh
if [ "${1:-}" = --protocol ]; then echo LX06DIALOGUE/1; exit 0; fi
exit "$CORE_STATUS"
CORE
chmod +x "$TEST_ROOT/core"
printf '%s' lease > "$TEST_ROOT/lease"
for scenario in clean warm socket unknown; do
    for code in 0 2 75 3; do
        mkdir "$TEST_ROOT/session"
        status=0
        FAIL_KIND=$scenario CORE_STATUS=$code "${TEST_SHELL:-sh}" "$TEST_ROOT/worker" > "$TEST_ROOT/out" 2> "$TEST_ROOT/err" || status=$?
        expected=$code
        if [ "$scenario" != clean ] && [ "$code" = 0 ]; then expected=1; fi
        [ "$status" = "$expected" ] || { printf 'unexpected status: %s %s => %s\n' "$scenario" "$code" "$status" >&2; exit 1; }
        [ "$(command cat "$TEST_ROOT/lease")" = lease ] || exit 1
        if [ "$scenario" = clean ]; then
            [ ! -e "$TEST_ROOT/session" ] && [ ! -s "$TEST_ROOT/err" ] || exit 1
        else
            if ! command grep -q 'account-voice: session cleanup incomplete' "$TEST_ROOT/err"; then
                printf 'missing cleanup diagnostic: %s core=%s\n' "$scenario" "$code" >&2; exit 1
            fi
            ! command grep -q "$TEST_ROOT" "$TEST_ROOT/err" || exit 1
            case "$scenario" in
                warm) [ -f "$TEST_ROOT/session/http-warm/method" ] || exit 1;;
                socket) [ -f "$TEST_ROOT/session/http.sock" ] || exit 1;;
                unknown) [ "$(command cat "$TEST_ROOT/session/unrelated")" = keep ] || exit 1;;
            esac
            command rm -rf "$TEST_ROOT/session"
        fi
    done
done
mkdir "$TEST_ROOT/session"
printf '%s' keep > "$TEST_ROOT/session/unrelated"
status=0
FAIL_KIND=mktemp CORE_STATUS=0 "${TEST_SHELL:-sh}" "$TEST_ROOT/worker" > "$TEST_ROOT/out" 2> "$TEST_ROOT/err" || status=$?
[ "$status" != 0 ] && [ "$(command cat "$TEST_ROOT/session/unrelated")" = keep ] && [ "$(command cat "$TEST_ROOT/lease")" = lease ] || exit 1
printf '%s\n' 'PASS worker EXIT cleanup: clean, rm failures, unknown file, mktemp failure; statuses and lease preserved'
