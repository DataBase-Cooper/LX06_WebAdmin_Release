#!/usr/bin/env bash
# build_webd_standalone.sh —— V23 独立 webd 构建（web88 独立进程 /usr/sbin/lx06webd）。
#
# 源码：
#   hooks/webd/webd.c             HTTP 控制台（入口 webd_main，standalone 自管理：
#                                 自日志重定向 + 单实例 flock + listening 日志）
#   hooks/webd/webd_standalone.c  main() 包装（调 webd_main）
#   hooks/robust/{shim225.c,stat225_shim.c}  原数值/stat glibc 2.25 符号桥
#   hooks/webd/app-target-settings.c + native-settings/ 原唯一联合设置事务
#   runtime/common/bridge-contract + dialogue-core/bridge 原 caps/TLS profile
# 链接参数与 unified 版 webd 部分同源：设备 glibc 2.25 动态 + shim 桥；
# MD5-crypt 内嵌（不链 libcrypt）。
#
# 用法:  scripts/build_webd_standalone.sh <out-binary>
# 环境:  需 arm-linux-gnueabihf-gcc（WSL）。
#   LX06_LIB           sysroot lib 目录（含 libc.so.6，推荐解包出的官方镜像 lib）
#   LX06_SYSROOT_LIB   同上（默认 hooks/unified/dropbear-2017.75/.sysroot-lib）
#   LX06_TLS_PREFIX    必须显式指定离线 ARM TLS 头/库前缀；include/openssl
#                      与 lib/{libssl.so.1.0.0,libcrypto.so.1.0.0}，不自动获取
#   LX06_FW_VERSION    固件版本号（webd /api/version 回退常量；镜像内
#                      /lx06-firmware-version 文件存在时优先用文件）
set -e
[ "$#" -eq 1 ] || { printf 'usage: %s <output-binary>\n' "$0" >&2; exit 2; }
mkdir -p "$(dirname "$1")"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OBJDIR=$(mktemp -d "${LX06_BUILD_TMPDIR:-$(dirname "$OUT")}/lx06-webd-XXXXXX")
trap 'rm -rf "$OBJDIR"' EXIT
REPO=$(cd "$(dirname "$0")/.." && pwd)
W=$REPO/hooks/webd
U=$REPO/hooks/unified
DB=$U/dropbear-2017.75
SYSROOT_LIB=${LX06_SYSROOT_LIB:-$DB/.sysroot-lib}
[ -n "$LX06_LIB" ] && SYSROOT_LIB=$LX06_LIB

: "${LX06_TLS_PREFIX:?explicit offline ARM TLS prefix required}"
TLS_PREFIX=$(cd "$LX06_TLS_PREFIX" && pwd)
TLS_INPUTS=(
  "$TLS_PREFIX/include/openssl/ssl.h" "$TLS_PREFIX/include/openssl/evp.h"
  "$TLS_PREFIX/include/openssl/sha.h" "$TLS_PREFIX/include/openssl/x509v3.h"
  "$TLS_PREFIX/lib/libssl.so.1.0.0" "$TLS_PREFIX/lib/libcrypto.so.1.0.0"
)
for input in "${TLS_INPUTS[@]}"; do
  [ -f "$input" ] && [ -r "$input" ] || { printf 'missing readable target TLS input: %s\n' "$input" >&2; exit 1; }
done

CC=${CC:-arm-linux-gnueabihf-gcc}
STRIP=${STRIP:-arm-linux-gnueabihf-strip}
NM=${NM:-arm-linux-gnueabihf-nm}
T32="-D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32"
XINC="-I$U/xcryptinc"
# These are the same normal Web/joint/profile translation units. webd.c owns
# the sole original JSON implementation; never add another json.c or matcher.
NATIVE=$W/native-settings
BRIDGE=$REPO/runtime/dialogue-core/bridge
COMMON=$REPO/runtime/common/bridge-contract
NATIVE_FLAGS=(
  -std=c11 -D_POSIX_C_SOURCE=200809L -DLX06_NATIVE_TARGET_SETTINGS=1
  -DLX06_NATIVE_TOKEN_SETTINGS=1 -DXZ_OPENSSL_1_0_COMPAT
  -Wno-unused-function -Wno-misleading-indentation -Wno-sign-compare -Wno-deprecated-declarations
  -I"$NATIVE" -I"$BRIDGE" -I"$REPO/runtime/doubao/src" -I"$TLS_PREFIX/include"
)
NATIVE_SOURCES=(
  "$W/app-target-settings.c"
  "$NATIVE/ctl-config.c" "$NATIVE/bridge-view.c" "$NATIVE/declaration-reader.c" "$NATIVE/token-sync-b16.c"
  "$COMMON/caps.c" "$COMMON/caps_json.c" "$COMMON/describe.c"
  "$BRIDGE/wire.c" "$BRIDGE/discovery.c" "$BRIDGE/operation.c" "$BRIDGE/session.c" "$BRIDGE/profile.c"
  "$REPO/runtime/dialogue-core/adapters/support/xiaozhi/ws_frame.c"
)
NATIVE_OBJECTS=()

echo "=== [1/2] 编译 webd + 原生联合设置/caps + standalone main + 原 ABI shims ==="
FWV=${LX06_FW_VERSION:-unknown}
$CC $T32 -c -O2 -fPIE "${NATIVE_FLAGS[@]}" $XINC -I"$REPO/hooks/robust" -DLX06_WEBD_STANDALONE -DLX06_FW_VERSION="\"$FWV\"" \
  -o "$OBJDIR/webd.o" "$W/webd.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/webd_standalone.o" "$W/webd_standalone.c"
# Numeric and stat wrappers must keep POSIX/native time32/off32 declarations;
# do not expose GNU/C23 redirects inside their own forwarding definitions.
$CC $T32 -D_POSIX_C_SOURCE=200809L -c -O2 -fPIE -o "$OBJDIR/shim225.o" "$REPO/hooks/robust/shim225.c"
$CC $T32 -D_POSIX_C_SOURCE=200809L -c -O2 -fPIE -o "$OBJDIR/stat225_shim.o" "$REPO/hooks/robust/stat225_shim.c"
for source in "${NATIVE_SOURCES[@]}"; do
  object=$OBJDIR/native-${#NATIVE_OBJECTS[@]}.o
  $CC $T32 -c -O2 -fPIE "${NATIVE_FLAGS[@]}" -o "$object" "$source"
  NATIVE_OBJECTS+=("$object")
done
$CC $T32 -c -O2 -fPIE -DARGTYPE=4 -DOTA_VERIFY_LIBRARY -I"$DB" -I"$DB/libtomcrypt/src/headers" \
  -o "$OBJDIR/ota-verify.o" "$REPO/hooks/upgrade/ota-verify.c"
$CC $T32 -c -O2 -fPIE -DARGTYPE=4 -I"$DB" -I"$DB/libtomcrypt/src/headers" \
  -o "$OBJDIR/ota-sha256.o" "$DB/libtomcrypt/src/hashes/sha2/sha256.c"
echo "objects ok (FWV=$FWV)"

echo "=== [2/2] 动态链接（设备 glibc 2.25）==="
[ -f "$SYSROOT_LIB/libc.so.6" ] || { echo "SYSROOT_LIB $SYSROOT_LIB 无 libc.so.6"; exit 1; }
# 解释器 = stock /lib/ld-linux-armhf.so.3（与 dropbear 同档，真机验证）
LDFLAGS="-pie -Wl,-z,now -Wl,-z,relro -Wl,--as-needed -L$SYSROOT_LIB -Wl,-rpath-link,$SYSROOT_LIB -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3"
SO="-l:libc.so.6 -l:libm.so.6 -l:libpthread.so.0 -l:libutil.so.1 -l:libgcc_s.so.1"
$CC $T32 $LDFLAGS -o "$OUT" \
  "$OBJDIR/webd_standalone.o" "$OBJDIR/webd.o" "$OBJDIR/shim225.o" "$OBJDIR/stat225_shim.o" \
  "${NATIVE_OBJECTS[@]}" "$OBJDIR/ota-verify.o" "$OBJDIR/ota-sha256.o" \
  "$TLS_PREFIX/lib/libssl.so.1.0.0" "$TLS_PREFIX/lib/libcrypto.so.1.0.0" \
  -Xlinker -rpath-link -Xlinker "$TLS_PREFIX/lib" $SO
echo "link ok"

echo "=== 产物 ==="
$STRIP "$OUT" 2>/dev/null || true
ls -la "$OUT"
file "$OUT" 2>/dev/null | sed 's/, statically.*//;s/, for GNU.*/ (dynamic)/'
echo "NEEDED:"; readelf -d "$OUT" | grep -E "NEEDED|RPATH|RUNPATH" | head
echo "=== 时间符号检查（应无 time64 / __isoc23_* 未定义引用）==="
$NM -D "$OUT" 2>/dev/null | grep -iE "time64|__isoc23" | head || true
echo "DONE $OUT"
