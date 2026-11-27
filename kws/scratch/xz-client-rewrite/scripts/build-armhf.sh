#!/usr/bin/env bash
# Build an LX06-compatible armhf binary against the device's glibc 2.25/OpenSSL 1.0 ABI.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CC="${CC:-arm-linux-gnueabihf-gcc}"
READELF="${READELF:-arm-linux-gnueabihf-readelf}"
NM="${NM:-arm-linux-gnueabihf-nm}"
LEGACY_LIB_DIR="${LEGACY_LIB_DIR:-$ROOT/build/arm-libs}"
OPUS_LIBRARY="${OPUS_LIBRARY:-$LEGACY_LIB_DIR/libopus.so.0.6.1}"
GCC_LIB_DIR="${GCC_LIB_DIR:-/usr/lib/gcc-cross/arm-linux-gnueabihf/12}"
CRT_DIR="${CRT_DIR:-/usr/arm-linux-gnueabihf/lib}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/armhf}"
OBJECT_DIR="$BUILD_DIR/obj"
OUTPUT="$BUILD_DIR/xz_client"

for required in \
    "$LEGACY_LIB_DIR/sysarm-libc-2.25.so" \
    "$LEGACY_LIB_DIR/sysarm-libpthread.so.0" \
    "$LEGACY_LIB_DIR/sysarm-libm-2.25.so" \
    "$LEGACY_LIB_DIR/sysarm-libdl.so.2" \
    "$LEGACY_LIB_DIR/sysarm-libgcc_s.so.1" \
    "$LEGACY_LIB_DIR/sysarm-libssl.so.1.0.0" \
    "$LEGACY_LIB_DIR/sysarm-libcrypto.so.1.0.0" \
    "$OPUS_LIBRARY" \
    "$CRT_DIR/Scrt1.o" "$CRT_DIR/crti.o" "$CRT_DIR/crtn.o" \
    "$GCC_LIB_DIR/crtbeginS.o" "$GCC_LIB_DIR/crtendS.o"; do
    [[ -r "$required" ]] || { echo "missing build input: $required" >&2; exit 2; }
done

rm -rf "$OBJECT_DIR"
mkdir -p "$OBJECT_DIR"

SOURCES=(
    src/aec.c src/event.c src/home_mcp.c src/identity.c src/json.c src/main.c src/opus_codec.c src/player.c
    src/protocol.c src/session.c src/talk.c src/task_poll.c src/text_match.c src/timeline.c src/ws.c src/ws_frame.c
    ../../lx06-installer/runtime/common/control_asr.c
    ../../lx06-installer/hooks/webd/shim225.c
    ../../lx06-installer/hooks/robust/stat225_shim.c
)
COMMON_FLAGS=(
    -Isrc -D_POSIX_C_SOURCE=200809L -D_TIME_BITS=32 -DXZ_OPENSSL_1_0_COMPAT
    -std=c11 -O2 -fPIE -Wall -Wextra -Werror
)
OBJECTS=()
for source in "${SOURCES[@]}"; do
    object="$OBJECT_DIR/$(basename "${source%.c}").o"
    "$CC" "${COMMON_FLAGS[@]}" -c "$source" -o "$object"
    OBJECTS+=("$object")
done

"$CC" -nostdlib -pie \
    -Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3 \
    -Wl,-z,relro,-z,now -Wl,--unresolved-symbols=ignore-in-shared-libs \
    "$CRT_DIR/Scrt1.o" "$CRT_DIR/crti.o" "$GCC_LIB_DIR/crtbeginS.o" \
    "${OBJECTS[@]}" \
    "$LEGACY_LIB_DIR/sysarm-libssl.so.1.0.0" \
    "$LEGACY_LIB_DIR/sysarm-libcrypto.so.1.0.0" \
    "$OPUS_LIBRARY" \
    "$LEGACY_LIB_DIR/sysarm-libpthread.so.0" \
    "$LEGACY_LIB_DIR/sysarm-libm-2.25.so" \
    "$LEGACY_LIB_DIR/sysarm-libdl.so.2" \
    "$LEGACY_LIB_DIR/sysarm-libc-2.25.so" \
    "$LEGACY_LIB_DIR/sysarm-libgcc_s.so.1" \
    -lgcc "$GCC_LIB_DIR/crtendS.o" "$CRT_DIR/crtn.o" \
    -o "$OUTPUT"

"$READELF" -h "$OUTPUT" | grep -q 'Machine:.*ARM'
for soname in libssl.so.1.0.0 libcrypto.so.1.0.0 libopus.so.0 libpthread.so.0 libc.so.6; do
    "$READELF" -d "$OUTPUT" | grep -Fq "Shared library: [$soname]" || {
        echo "missing expected runtime dependency: $soname" >&2
        exit 3
    }
done
if "$NM" -D --undefined-only "$OUTPUT" | grep -Eq \
    'TLS_client_method|OPENSSL_sk_|ASN1_STRING_get0_data|SSL_get1_peer_certificate'; then
    echo 'armhf artifact imports an OpenSSL 1.1+/3.x symbol unavailable on LX06' >&2
    exit 4
fi
if "$READELF" --dyn-syms -W "$OUTPUT" | grep -Eq '@GLIBC_2\.(2[6-9]|[3-9][0-9])'; then
    echo 'armhf artifact requires glibc newer than the LX06 2.25 runtime' >&2
    exit 5
fi

echo "ARMHF_BUILD_OK output=$OUTPUT"
sha256sum "$OUTPUT"
