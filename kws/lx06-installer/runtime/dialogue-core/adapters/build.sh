#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mode=${1:-host}
case "$mode" in host|arm) ;; *) exit 2;; esac
out=${DC_ADAPTER_BUILD_DIR:-build/$mode}
if [ -e "$out/lx06-transport-adapter" ]; then echo immutable-build-output-exists >&2;exit 2;fi
mkdir -p "$out/obj"
sources=(worker.c transport.c ../protocol.c support/common/json.c support/common/ogg_audio.c
 support/xiaozhi/protocol.c support/xiaozhi/event.c support/xiaozhi/opus_codec.c
 support/xiaozhi/ws.c support/xiaozhi/ws_frame.c support/doubao/ws.c support/doubao/ws_frame.c)
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -Isupport/common -Isupport/xiaozhi)
if [ "${DC_ADAPTER_SANITIZE:-0}" = 1 ]; then
 [ "$mode" = host ] || exit 2
 flags+=(-O1 -fsanitize=address,undefined -fno-sanitize-recover=all)
fi
if [ "$mode" = host ]; then
 gcc "${flags[@]}" "${sources[@]}" -o "$out/lx06-transport-adapter" -lssl -lcrypto -lopus
else
 legacy=${LX06_LEGACY_LIB:-/opt/lx06/legacy}
 opus=${OPUS_LIBRARY:-/opt/lx06/legacy/libopus.so.0.6.1}
 crt=/usr/arm-linux-gnueabihf/lib
 gccdir=$(dirname "$(arm-linux-gnueabihf-gcc -print-file-name=crtbeginS.o)")
 sources+=(../../../hooks/webd/shim225.c)
 objects=();index=0
 for source in "${sources[@]}"; do
  object="$out/obj/$index.o";index=$((index+1))
  arm-linux-gnueabihf-gcc "${flags[@]}" -I/usr/include -D_TIME_BITS=32 -DXZ_OPENSSL_1_0_COMPAT -fPIE -c "$source" -o "$object"
  objects+=("$object")
 done
 arm-linux-gnueabihf-gcc -nostdlib -pie -Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3 \
  -Wl,-z,relro,-z,now -Wl,--unresolved-symbols=ignore-in-shared-libs \
  "$crt/Scrt1.o" "$crt/crti.o" "$gccdir/crtbeginS.o" "${objects[@]}" \
  "$opus" "$legacy/sysarm-libssl.so.1.0.0" "$legacy/sysarm-libcrypto.so.1.0.0" \
  "$legacy/sysarm-libm-2.25.so" "$legacy/sysarm-libdl.so.2" "$legacy/sysarm-libc-2.25.so" \
  "$legacy/sysarm-libgcc_s.so.1" -lgcc "$gccdir/crtendS.o" "$crt/crtn.o" -o "$out/lx06-transport-adapter"
 arm-linux-gnueabihf-strip "$out/lx06-transport-adapter"
 if arm-linux-gnueabihf-readelf --dyn-syms -W "$out/lx06-transport-adapter" | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
  echo unsupported-stock-glibc >&2;exit 1
 fi
fi
python3 - "$mode" "$out" "${sources[@]}" <<'PY'
import hashlib,json,sys
from pathlib import Path
mode,out,*sources=sys.argv[1:];out=Path(out)
paths=set(sources)|{str(p) for p in Path('support').rglob('*.h')}|{'transport.h','../protocol.h','../types.h','build.sh','support-origin.json','support-local-deltas.json'}
digest=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
binary=out/'lx06-transport-adapter'
import os
(out/'source-reference.json').write_text(json.dumps(dict(schema=1,mode=mode,sanitized=os.environ.get('DC_ADAPTER_SANITIZE')=='1',binary=dict(sha256=digest(binary),bytes=binary.stat().st_size),sources={p:digest(p) for p in sorted(paths)}),indent=2)+'\n')
PY
sha256sum "$out/lx06-transport-adapter"
