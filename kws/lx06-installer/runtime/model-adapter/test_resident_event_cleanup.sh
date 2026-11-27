#!/bin/sh
# Run as an unprivileged user on a private POSIX filesystem with TMPDIR set.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$(id -u)" != 0 ] || { echo 'requires unprivileged uid' >&2; exit 1; }
T=$(mktemp -d "${TMPDIR:?}/resident-cleanup.XXXXXX")
trap 'chmod 700 "$T/eventdir" 2>/dev/null || :; rm -rf "$T"' EXIT
mkdir "$T/eventdir"
printf '%s' untouched > "$T/eventdir/unrelated"
export T
# Extract real functions; relocate only the two hard-coded paths into this fixture.
awk '/^# Audio components come from the unified firmware\./ {exit} {gsub("/tmp/lx06-wake-", "$T/eventdir/lx06-wake-"); gsub("/tmp/lx06-\\$PROVIDER-resident.lock", "$T/lock"); print}' "$HERE/account-voice.sh" > "$T/resident"
cat >> "$T/resident" <<'SCRIPT'
voice_wait_init() { return 0; }
voice_wait_event() {
    printf '%s' private-fixture > "$EVENT"
    stat -c '%u' "$EVENT" > "$T/event-owner"
    if [ "$FAIL_CLEANUP" = yes ]; then chmod 500 "$T/eventdir"; fi
    if [ "$TEST_EXIT" = term ]; then kill -TERM "$$"; else return 42; fi
}
PROVIDER=fixture
voice_resident
SCRIPT
for scenario in clean-term clean-status fail-term fail-status; do
    chmod 700 "$T/eventdir"
    case $scenario in clean-term) fail=no; how=term; expected=0;; clean-status) fail=no; how=status; expected=42;; fail-term) fail=yes; how=term; expected=1;; fail-status) fail=yes; how=status; expected=42;; esac
    status=0
    FAIL_CLEANUP=$fail TEST_EXIT=$how "${TEST_SHELL:-sh}" "$T/resident" > "$T/out" 2> "$T/err" || status=$?
    [ "$(cat "$T/event-owner")" = "$(id -u)" ] || { echo 'wrong fixture owner' >&2; exit 1; }
    if [ "$status" != "$expected" ]; then printf '%s status=%s expected=%s\n' "$scenario" "$status" "$expected" >&2; exit 1; fi
    if [ "$fail" = yes ]; then
        [ -f "$T/eventdir/lx06-wake-resident.event" ] || { echo 'failed event missing' >&2; exit 1; }
        grep -q '^account-voice: resident event cleanup incomplete$' "$T/err" || { echo 'missing safe diagnostic' >&2; exit 1; }
        ! grep -F -q "$T" "$T/err" || { echo 'diagnostic leaked path' >&2; exit 1; }
        chmod 700 "$T/eventdir"
        rm "$T/eventdir/lx06-wake-resident.event"
    else
        [ ! -e "$T/eventdir/lx06-wake-resident.event" ] && [ ! -s "$T/err" ] || exit 1
    fi
    [ "$(cat "$T/eventdir/unrelated")" = untouched ] || { echo 'unrelated fixture changed' >&2; exit 1; }
    printf '%s status=%s event-owner=running-uid\n' "$scenario" "$status"
done
