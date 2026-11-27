#!/usr/bin/env bash
# Provider-neutral helper; use stock 2.25 ABI and the speaker's libcrypto 1.0.
set -euo pipefail
cd "$(dirname "$0")"
out=${1:?output path required}
source=${2:-model-tool.c}
case "$source" in model-tool.c|model-audio.c|model-asr.c|model-voice.c|model-http.c|script-settings.c|deepseek-stream.c|account-wait.c) ;; *) echo 'Unknown helper source' >&2; exit 2;; esac
mkdir -p "$(dirname "$out")"
legacy=${LX06_LEGACY_LIB:-/opt/lx06/legacy}
crt=/usr/arm-linux-gnueabihf/lib
gccdir=$(dirname "$(arm-linux-gnueabihf-gcc -print-file-name=crtbeginS.o)")
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
warnings=(-Werror=implicit-function-declaration)
if [ "$source" = account-wait.c ]; then warnings+=(-std=gnu11 -Wall -Wextra -Werror); fi
arm-linux-gnueabihf-gcc -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 "${warnings[@]}" -O2 -fPIE -c "$source" -o "$obj/tool.o"
extra=()
if [ "$source" = model-asr.c ] || [ "$source" = model-voice.c ]; then
  for item in ../common/control_asr.c ../doubao/src/json.c ../doubao/src/text_match.c; do
    name=$(basename "${item%.c}")
    arm-linux-gnueabihf-gcc -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -O2 -fPIE -c "$item" -o "$obj/$name.o"
    extra+=("$obj/$name.o")
  done
  extra+=("${OPUS_LIBRARY:-/opt/lx06/legacy/libopus.so.0.6.1}" "$legacy/sysarm-libm-2.25.so")
fi
support=()
libraries=()
link_flags=()
if [ "$source" != account-wait.c ]; then
  arm-linux-gnueabihf-gcc -D_TIME_BITS=32 -O2 -fPIE -c ../../hooks/webd/shim225.c -o "$obj/shim.o"
  support+=("$obj/shim.o")
  libraries+=("$legacy/sysarm-libcrypto.so.1.0.0" "$legacy/sysarm-libdl.so.2")
else
  # LX06 uses 4KiB pages; avoid a toolchain-default 64KiB file alignment hole
  # dominating this tiny wait-only executable. Other helper links stay intact.
  link_flags+=(-Wl,-z,max-page-size=4096)
fi
# The standalone waiter needs only stock libc/libgcc. It must never gain an
# audio/ASR/Opus/crypto/SDK dependency when packed beside the unified core.
arm-linux-gnueabihf-gcc -nostdlib -pie -Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3 \
  -Wl,-z,relro,-z,now -Wl,--unresolved-symbols=ignore-in-shared-libs \
  "${link_flags[@]}" \
  "$crt/Scrt1.o" "$crt/crti.o" "$gccdir/crtbeginS.o" "$obj/tool.o" "${support[@]}" "${extra[@]}" \
  "${libraries[@]}" \
  "$legacy/sysarm-libc-2.25.so" "$legacy/sysarm-libgcc_s.so.1" -lgcc \
  "$gccdir/crtendS.o" "$crt/crtn.o" -o "$out"
arm-linux-gnueabihf-strip "$out"
if arm-linux-gnueabihf-readelf --dyn-syms -W "$out" | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
  echo 'Unsupported glibc ABI' >&2; exit 1
fi
sha256sum "$out"
