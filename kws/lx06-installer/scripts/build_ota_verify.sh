#!/bin/sh
# Build the read-only ARM verifier. No deployment or release packaging.
set -eu
[ "$#" = 1 ] || { printf 'usage: %s NEW_OUTPUT\n' "$0" >&2; exit 2; }
[ ! -e "$1" ] && [ ! -L "$1" ] || { printf 'output exists; refusing overwrite\n' >&2; exit 1; }
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENDOR="$ROOT/hooks/unified/dropbear-2017.75"
# Static verifier intentionally has no target-shared-library dependency.
# mktemp stays alongside the explicit output, not system TMP.
BUILD=$(mktemp -d "$(dirname -- "$1")/.ota-build-XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT HUP INT TERM
"${CC:-arm-linux-gnueabihf-gcc}" -O2 -Wall -Wextra -Werror -static -DARGTYPE=4 \
    -I"$VENDOR" -I"$VENDOR/libtomcrypt/src/headers" \
    "$ROOT/hooks/upgrade/ota-verify.c" \
    "$VENDOR/libtomcrypt/src/hashes/sha2/sha256.c" -o "$BUILD/lx06-ota-verify"
# Hardlink publication refuses a destination created by another build.
ln -- "$BUILD/lx06-ota-verify" "$1"
