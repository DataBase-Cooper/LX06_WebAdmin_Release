#!/usr/bin/env bash
# build_unified.sh —— V22 统一二进制自包含构建（SSH 89 + HTTP 88 单 ELF）。
#
# 从 repo 内 vendor 的源码树构建，不依赖 /root 下的开发副本：
#   hooks/unified/dropbear-2017.75/   改过的 dropbear 2017.75 树
#                                     （lx06_unified.c/h 双端口集成 +
#                                       svr-main/svr-session/common-runopts 的 LX06 改动 +
#                                       预编译 libtomcrypt/libtommath 静态库 + configure 产物）
#   hooks/unified/libz.a              静态 zlib（无 time64 符号）
#   hooks/unified/zlibinc/            zlib 头
#   hooks/unified/xcryptinc/          crypt.h 头
#   hooks/unified/pamstub2.c          svr_auth_pam 桩（直接 crypt 认证，不链 libpam）
#   hooks/unified/shim_strl.c         strlcpy/strlcat 桩
#   hooks/webd/webd.c                 webd（HTTP 控制台，main→webd_main）
#   hooks/webd/shim225.c              glibc 2.41 头→2.25 符号桥
#
# 用法:  scripts/build_unified.sh <out-binary>
# 环境:  需 arm-linux-gnueabihf-gcc（WSL 或设备工具链）。
# 链接库: 设备自带 glibc 2.25（-L <sysroot>/lib）。sysroot 由 $LX06_SYSROOT_LIB
#   指定（默认 $WORK/lib，即 build_ssh_base.sh 解包出的官方镜像 lib —— 与运行库
#   同源，md5 已验证一致）。
set -e
[ "$#" -eq 1 ] || { printf 'usage: %s <output-binary>\n' "$0" >&2; exit 2; }
mkdir -p "$(dirname "$1")"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
# 编译对象在调用方指定的输出目录旁，不使用全局 /tmp 固定名字互相踩写。
OBJDIR=$(mktemp -d "${LX06_BUILD_TMPDIR:-$(dirname "$OUT")}/lx06-unified-XXXXXX")
trap 'rm -rf "$OBJDIR"' EXIT
REPO=$(cd "$(dirname "$0")/.." && pwd)
U=$REPO/hooks/unified
DB=$U/dropbear-2017.75
W=$REPO/hooks/webd
SYSROOT_LIB=${LX06_SYSROOT_LIB:-$DB/.sysroot-lib}
# 若未指定 sysroot，且提供了 LX06_LIB 目录（含 libc.so.6），用它
[ -n "$LX06_LIB" ] && SYSROOT_LIB=$LX06_LIB

CC=${CC:-arm-linux-gnueabihf-gcc}
AR=${AR:-arm-linux-gnueabihf-ar}
STRIP=${STRIP:-arm-linux-gnueabihf-strip}
NM=${NM:-arm-linux-gnueabihf-nm}
T32="-D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32"
ZINC="-I$U/zlibinc"
XINC="-I$U/xcryptinc"

cd "$DB"
echo "=== [1/5] 清理旧 .o ==="
rm -f *.o

echo "=== [2/5] 重编 dropbear 全部对象（_TIME_BITS=32，消除 64 位 time syscall）==="
COMMONOBJS="dbutil.o buffer.o dbhelpers.o dss.o bignum.o signkey.o rsa.o dbrandom.o queue.o atomicio.o compat.o fake-rfc2553.o ltc_prng.o ecc.o ecdsa.o crypto_desc.o gensignkey.o gendss.o genrsa.o"
SVROBJS="svr-kex.o svr-auth.o sshpty.o svr-authpasswd.o svr-authpubkey.o svr-authpubkeyoptions.o svr-session.o svr-service.o svr-chansession.o svr-runopts.o svr-agentfwd.o svr-main.o svr-x11fwd.o svr-tcpfwd.o svr-authpam.o"
CLISVROBJS="common-session.o packet.o common-algo.o common-kex.o common-channel.o common-chansession.o termcodes.o loginrec.o tcp-accept.o listener.o process-packet.o dh_groups.o common-runopts.o circbuffer.o curve25519-donna.o list.o netio.o"
ALL="$COMMONOBJS $SVROBJS $CLISVROBJS"
CF="$T32 -c -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -Wundef -fno-strict-overflow -Wno-pointer-sign -O2 -std=gnu99 -I$ZINC -I$XINC -I$DB/libtomcrypt/src/headers/ -I. -I. -DDROPBEAR_SERVER -DDROPBEAR_CLIENT -DHAVE_CRYPT -DLX06_UNIFIED"
for src in $ALL; do
  c="${src%.o}.c"
  [ -e "$c" ] || { echo "MISS src $c"; continue; }
  $CC $CF -o "$src" "$c"
done
$CC $CF -o lx06_unified.o lx06_unified.c && echo "lx06_unified.o ok"
echo "dropbear objects: $(echo $ALL | wc -w)"

echo "=== [3/5] webd + pamstub + shim（_TIME_BITS=32）==="
# keeper_client.h / keeper_args.h 与当前 webd 同源，避免链接进旧 root 执行路径。
# LX06_FW_VERSION（环境变量）：webd /api/version 的回退常量；镜像内
# /lx06-firmware-version 文件存在时优先用文件（单一来源）。
FWV=${LX06_FW_VERSION:-unknown}
$CC $T32 -c -O2 -fPIE $XINC -I"$REPO/hooks/robust" -DLX06_WEBD_INTEGRATED -DLX06_FW_VERSION="\"$FWV\"" -o "$OBJDIR/webd_integrated.o" "$W/webd.c"
$CC $T32 -c -O2 -fPIE -DARGTYPE=4 -DOTA_VERIFY_LIBRARY -I"$DB" -I"$DB/libtomcrypt/src/headers" \
  -o "$OBJDIR/ota-verify.o" "$REPO/hooks/upgrade/ota-verify.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/pamstub2.o" "$U/pamstub2.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/shim_strl.o" "$U/shim_strl.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/shim225.o" "$W/shim225.c"

echo "=== [4/5] 静态库 time64 符号检查（应=0）==="
for a in $U/libz.a $DB/libtomcrypt/libtomcrypt.a $DB/libtommath/libtommath.a; do
  n=$($AR t "$a" >/dev/null 2>&1 && $NM "$a" 2>/dev/null | grep -cE "U __isoc23|U .time64" || echo 0)
  echo "  $(basename $a) time64-U=$n"
done

echo "=== [5/5] 动态链接（设备 glibc 2.25 .so + 静态 zlib/tom）==="
[ -f "$SYSROOT_LIB/libc.so.6" ] || { echo "SYSROOT_LIB $SYSROOT_LIB 无 libc.so.6"; exit 1; }
# 解释器必须用 stock /lib/ld-linux-armhf.so.3（V14.2 同款，真机验证）：
# 曾改为 /data/glibc/lib/ld-linux-armhf.so.3，但镜像/刷机/启动链都没有任何
# 步骤往 /data 写 glibc —— 用户清空 /data 刷机后内核加载解释器 ENOENT，
# S50dropbear 无限 respawn，88/89 全程不监听 → S70 看门狗 120s 重启死循环
# （2026-10-17 .166 实机复现）。本 ELF 未定义符号集与 V14.2 完全同档，
# stock glibc 2.25 全部满足（objdump 比对 + QEMU 启动实测）。
LDFLAGS="-pie -Wl,-z,now -Wl,-z,relro -Wl,--as-needed -L$SYSROOT_LIB -Wl,-rpath-link,$SYSROOT_LIB -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3"
SO="-l:libc.so.6 -l:libcrypt.so.1 -l:libm.so.6 -l:libpthread.so.0 -l:libutil.so.1 -l:libgcc_s.so.1"
STATIC="$DB/libtomcrypt/libtomcrypt.a $DB/libtommath/libtommath.a $U/libz.a"
OBJLIST="$COMMONOBJS $SVROBJS $CLISVROBJS lx06_unified.o"
$CC $T32 $LDFLAGS -o "$OUT" $OBJLIST "$OBJDIR/webd_integrated.o" \
  "$OBJDIR/pamstub2.o" "$OBJDIR/shim_strl.o" "$OBJDIR/shim225.o" "$OBJDIR/ota-verify.o" $STATIC $SO
echo "link ok"

echo "=== 产物 ==="
$STRIP "$OUT" 2>/dev/null || true
ls -la "$OUT"
file "$OUT" 2>/dev/null | sed 's/, statically.*//;s/, for GNU.*/ (dynamic)/'
echo "NEEDED:"; readelf -d "$OUT" | grep -E "NEEDED|RPATH|RUNPATH" | head
echo "=== 时间符号检查（应无 time64 / __isoc23_* 未定义引用）==="
$NM -D "$OUT" 2>/dev/null | grep -iE "time64|__isoc23|clock_gettime" | head || true
echo "DONE $OUT"
