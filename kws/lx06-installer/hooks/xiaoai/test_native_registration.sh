#!/bin/sh
# Run offline on Linux with TMPDIR pointing to isolated POSIX scratch.
set -eu
: "${TMPDIR:?set TMPDIR to isolated POSIX scratch}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
d=$(mktemp -d "$TMPDIR/native-reg-build-XXXXXX")
trap 'rm -rf -- "$d"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -pthread \
    -o "$d/test" "$here/test_native_registration.c" -ldl
"$d/test"
set +e
"$d/test" --require-gated >"$d/red" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ] || ! grep -F 'b.calls==0' "$d/red" >/dev/null; then
    cat "$d/red" >&2
    printf '%s\n' 'Expected gating RED was not observed' >&2
    exit 1
fi
printf '%s\n' 'Expected RED: second registration is not gated'
