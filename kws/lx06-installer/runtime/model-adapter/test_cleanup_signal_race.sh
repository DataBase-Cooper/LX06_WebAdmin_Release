#!/bin/sh
# Offline signal injection into the real worker and resident functions.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:?}/cleanup-race.XXXXXX")
trap 'rm -rf "$T"' EXIT
export T
awk '/^# Audio components come from the unified firmware\./ {exit} {gsub("/tmp/lx06-wake-", "$T/eventdir/lx06-wake-"); gsub("/tmp/lx06-\\$PROVIDER-resident.lock", "$T/lock"); print}' "$HERE/account-voice.sh" > "$T/functions"
cat > "$T/core" <<'CORE'
#!/bin/sh
[ "${1:-}" != --protocol ] || { echo LX06DIALOGUE/1; exit 0; }
exit 42
CORE
chmod +x "$T/core"
for kind in worker resident; do
  cp "$T/functions" "$T/$kind"
  cat >> "$T/$kind" <<'FIXTURE'
rm() {
    if [ "${ARMED:-}" = yes ]; then
        ARMED=no
        kill -TERM "$$"
    fi
    command rm "$@"
}
mktemp() { mkdir "$T/session"; printf '%s\n' "$T/session"; }
voice_http_start() { ARMED=yes; }
voice_wait_init() { return 0; }
voice_wait_event() { ARMED=yes; return 42; }
MODEL_ID=fixture
LX06_DIALOGUED=$T/core
PROVIDER=fixture
if [ "$KIND" = worker ]; then voice_worker; else voice_resident; fi
FIXTURE
done
mkdir "$T/eventdir"
failed=0
for kind in worker resident; do
  status=0
  KIND=$kind "${TEST_SHELL:-sh}" "$T/$kind" > "$T/out" 2> "$T/err" || status=$?
  if [ "$status" != 42 ]; then printf 'FAIL %s TERM during cleanup status=%s expected=42\n' "$kind" "$status" >&2; failed=1; else printf 'PASS %s TERM during cleanup status=42\n' "$kind"; fi
done
exit "$failed"
