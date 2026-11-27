#!/bin/sh
# Offline Linux, with isolated POSIX TMPDIR and no device access.
set -eu
: "${TMPDIR:?set TMPDIR to isolated POSIX scratch}"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
d=$(mktemp -d "$TMPDIR/native-variants-build-XXXXXX")
trap 'rm -rf -- "$d"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -DNDEBUG -pthread -o "$d/test" "$here/test_native_registration_variants.c" -ldl
for model in replace append ignore-null same-fn; do "$d/test" "$model"; done
