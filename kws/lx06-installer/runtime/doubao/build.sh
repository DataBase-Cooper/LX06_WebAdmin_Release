#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/obj
sources=(src/main.c src/json.c src/ws.c src/ws_frame.c src/player.c src/echo.c src/text_match.c src/ogg_audio.c ../common/control_asr.c)
if [ "${1:-}" = host ]; then
  gcc -D_POSIX_C_SOURCE=200809L -std=c11 -O2 -g -Wall -Wextra -Werror -Isrc "${sources[@]}" -o build/doubao-host -lssl -lcrypto -lpthread -lopus
  exit
fi
legacy=${LX06_LEGACY_LIB:-/opt/lx06/legacy}
# Modern cross headers redirect strtol to C23 symbols absent on stock 2.25.
# Use the same narrow ABI shim as the firmware helpers, never a newer libc.
sources+=(../../hooks/webd/shim225.c)
crt=/usr/arm-linux-gnueabihf/lib
gccdir=$(dirname "$(arm-linux-gnueabihf-gcc -print-file-name=crtbeginS.o)")
objects=()
for source in "${sources[@]}"; do
  object=build/obj/$(basename "${source%.c}").o
  arm-linux-gnueabihf-gcc -Isrc -D_POSIX_C_SOURCE=200809L -D_TIME_BITS=32 -DXZ_OPENSSL_1_0_COMPAT \
    -std=c11 -O2 -fPIE -Wall -Wextra -Werror -c "$source" -o "$object"
  objects+=("$object")
done
arm-linux-gnueabihf-gcc -nostdlib -pie -Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3 \
  -Wl,-z,relro,-z,now -Wl,--unresolved-symbols=ignore-in-shared-libs \
  "$crt/Scrt1.o" "$crt/crti.o" "$gccdir/crtbeginS.o" "${objects[@]}" \
  "${OPUS_LIBRARY:-/opt/lx06/legacy/libopus.so.0.6.1}" "$legacy/sysarm-libssl.so.1.0.0" "$legacy/sysarm-libcrypto.so.1.0.0" \
  "$legacy/sysarm-libpthread.so.0" "$legacy/sysarm-libm-2.25.so" "$legacy/sysarm-libdl.so.2" \
  "$legacy/sysarm-libc-2.25.so" "$legacy/sysarm-libgcc_s.so.1" \
  -lgcc "$gccdir/crtendS.o" "$crt/crtn.o" -o build/doubao-client
arm-linux-gnueabihf-strip build/doubao-client
if arm-linux-gnueabihf-readelf --dyn-syms -W build/doubao-client | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
  echo 'unsupported glibc ABI' >&2; exit 1
fi
sha256sum build/doubao-client
