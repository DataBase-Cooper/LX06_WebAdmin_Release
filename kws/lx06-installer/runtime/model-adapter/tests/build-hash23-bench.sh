#!/usr/bin/env bash
# Test-only old/new synthetic solver benchmark; no device deployment.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output path required}
legacy=${LX06_LEGACY_LIB:-/opt/lx06/legacy}
crt=/usr/arm-linux-gnueabihf/lib
gccdir=$(dirname "$(arm-linux-gnueabihf-gcc -print-file-name=crtbeginS.o)")
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
mkdir -p "$(dirname "$out")"
arm-linux-gnueabihf-gcc -D_TIME_BITS=32 -std=c11 -Wall -Wextra -Werror -O2 -fPIE -c tests/hash23-bench.c -o "$obj/bench.o"
arm-linux-gnueabihf-gcc -nostdlib -pie -Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3 -Wl,-z,relro,-z,now -Wl,--unresolved-symbols=ignore-in-shared-libs \
    "$crt/Scrt1.o" "$crt/crti.o" "$gccdir/crtbeginS.o" "$obj/bench.o" \
    "$legacy/sysarm-libc-2.25.so" "$legacy/sysarm-libgcc_s.so.1" \
    -lgcc "$gccdir/crtendS.o" "$crt/crtn.o" -o "$out"
arm-linux-gnueabihf-strip "$out"
if arm-linux-gnueabihf-readelf --dyn-syms -W "$out" | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])';then echo 'Unsupported ABI' >&2;exit 1;fi
sha256sum "$out"
