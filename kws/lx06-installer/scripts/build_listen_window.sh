#!/bin/sh
# ARM listen-window helper. Links the static libopus built by fetch_libopus.sh
# the same way model-asr links control_asr.c + json.c + opus, except the stock
# shared libopus.so.0.6.1 is not in this tree, so the static archive is used.
set -eu
cd "$(dirname "$0")/.."
mkdir -p hooks/xiaoai/build
CC=${CC:-arm-linux-gnueabihf-gcc}
PREFIX=$(sh scripts/fetch_libopus.sh)
OUT=hooks/xiaoai/build/lx06-listen-window
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -Wall -Wextra -static \
  -I hooks/xiaoai -I hooks/robust -I "$PREFIX/include" \
  -o "$OUT" \
  hooks/xiaoai/lx06-listen-window.c \
  hooks/xiaoai/listen-window.c \
  hooks/xiaoai/template-matcher.c \
  runtime/common/control_asr.c \
  runtime/doubao/src/json.c \
  "$PREFIX/lib/libopus.a" -lm
arm-linux-gnueabihf-strip "$OUT"
file "$OUT"
arm-linux-gnueabihf-readelf -h "$OUT"
sha256sum "$OUT"
