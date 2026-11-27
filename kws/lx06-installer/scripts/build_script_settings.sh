#!/usr/bin/env bash
# The small generic helper belongs in ROM; schemas/values stay with user scripts.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?output binary required}
lib=${LX06_LIB:?stock firmware lib directory required}
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
cc=${CC:-arm-linux-gnueabihf-gcc}
flags=(-D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -fPIE)
"$cc" "${flags[@]}" -c "$repo/runtime/model-adapter/script-settings.c" -o "$obj/settings.o"
"$cc" "${flags[@]}" -c "$repo/hooks/webd/shim225.c" -o "$obj/shim.o"
mkdir -p "$(dirname "$out")"
"$cc" -pie -Wl,-z,now,-z,relro,--as-needed -L"$lib" -Wl,-rpath-link,"$lib" \
  -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 "$obj/settings.o" "$obj/shim.o" \
  -l:libc.so.6 -l:libgcc_s.so.1 -o "$out"
arm-linux-gnueabihf-strip "$out"
if arm-linux-gnueabihf-readelf --dyn-syms -W "$out" | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
  echo 'Unsupported glibc ABI' >&2; exit 1
fi
