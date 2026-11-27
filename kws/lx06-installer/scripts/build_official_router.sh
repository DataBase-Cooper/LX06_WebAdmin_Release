#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p hooks/xiaoai/build
CC=${CC:-arm-linux-gnueabihf-gcc}
LIB=${LX06_SYSROOT_LIB:-devlib}
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -Wall -Wextra -static \
  -o hooks/xiaoai/build/official-router hooks/xiaoai/official-router.c runtime/doubao/src/json.c
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -Wall -Wextra -fPIC -shared -nostdlib \
  -Wl,-z,now -Wl,-z,relro -Wl,-z,defs -L"$LIB" -Wl,-rpath-link,"$LIB" \
  -o hooks/xiaoai/build/official-play-gate.so hooks/xiaoai/official-play-gate.c \
  -l:libc.so.6 -l:libdl-2.25.so
arm-linux-gnueabihf-strip hooks/xiaoai/build/official-router hooks/xiaoai/build/official-play-gate.so
arm-linux-gnueabihf-readelf --version-info hooks/xiaoai/build/official-play-gate.so
