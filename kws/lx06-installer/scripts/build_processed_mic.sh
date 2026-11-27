#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p hooks/xiaoai/build
CC=${CC:-arm-linux-gnueabihf-gcc}
LIB=${LX06_SYSROOT_LIB:-hooks/unified/dropbear-2017.75/.sysroot-lib}
[ -f "$LIB/libc.so.6" ] && [ -f "$LIB/libdl.so.2" ] || { echo 'stock libc/libdl sysroot required' >&2; exit 1; }
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -DLX06_ACCOUNT_MIC_LXA2 -O2 -fPIC -shared -nostdlib \
  -Wl,-z,now -Wl,-z,relro -Wl,-z,defs -L"$LIB" -Wl,-rpath-link,"$LIB" \
  -o hooks/xiaoai/build/processed-mic-tap.so hooks/xiaoai/processed-mic-tap.c hooks/xiaoai/native-wake-bridge.c \
  -l:libc.so.6 -l:libdl.so.2 -l:libpthread.so.0
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -static \
  -o hooks/xiaoai/build/processed-mic-read hooks/xiaoai/processed-mic-read.c
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -static -DLX06_SESSION_PHONE_RUNTIME \
  -o hooks/xiaoai/build/session-run hooks/xiaoai/session-run.c runtime/dialogue-core/mode_gate.c
$CC -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -Os -static \
  -o hooks/xiaoai/build/lx06-dialog-light hooks/xiaoai/dialog-light.c
arm-linux-gnueabihf-strip hooks/xiaoai/build/processed-mic-tap.so hooks/xiaoai/build/processed-mic-read hooks/xiaoai/build/session-run hooks/xiaoai/build/lx06-dialog-light
arm-linux-gnueabihf-readelf --version-info hooks/xiaoai/build/processed-mic-tap.so
