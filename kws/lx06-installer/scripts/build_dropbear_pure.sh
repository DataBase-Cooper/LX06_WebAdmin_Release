#!/usr/bin/env bash
# build_dropbear_pure.sh —— V23 纯 SSH dropbear 构建（只管 89，无 webd 集成）。
#
# 与 build_unified.sh 同源码树（hooks/unified/dropbear-2017.75），区别：
#   - 不带 -DLX06_UNIFIED（svr-main/svr-session 的 webd fork/看门狗/实例锁
#     全部 #ifdef 掉）
#   - 不编 lx06_unified.o、不链 webd 对象
#   - 认证仍是 svr_auth_password() 直接 crypt 校验 /etc/shadow（HAVE_CRYPT，
#     设备 Mico PAM 与 dropbear 2017.75 pamConv 不匹配 → 不走 PAM）
# 产物装为 /usr/sbin/dropbear；web88 由独立 /usr/sbin/lx06webd 承担。
#
# 用法:  scripts/build_dropbear_pure.sh <out-binary>
# 环境:  需 arm-linux-gnueabihf-gcc（WSL）。sysroot 由 $LX06_SYSROOT_LIB 指定
#   （默认 hooks/unified/dropbear-2017.75/.sysroot-lib）；LX06_LIB 优先。
set -e
[ "$#" -eq 1 ] || { printf 'usage: %s <output-binary>\n' "$0" >&2; exit 2; }
mkdir -p "$(dirname "$1")"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OBJDIR=$(mktemp -d "${LX06_BUILD_TMPDIR:-$(dirname "$OUT")}/lx06-pure-XXXXXX")
trap 'rm -rf "$OBJDIR"' EXIT
REPO=$(cd "$(dirname "$0")/.." && pwd)
U=$REPO/hooks/unified
DB=$U/dropbear-2017.75
SYSROOT_LIB=${LX06_SYSROOT_LIB:-$DB/.sysroot-lib}
[ -n "$LX06_LIB" ] && SYSROOT_LIB=$LX06_LIB

CC=${CC:-arm-linux-gnueabihf-gcc}
AR=${AR:-arm-linux-gnueabihf-ar}
STRIP=${STRIP:-arm-linux-gnueabihf-strip}
NM=${NM:-arm-linux-gnueabihf-nm}
T32="-D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32"
ZINC="-I$U/zlibinc"
XINC="-I$U/xcryptinc"

cd "$DB"
echo "=== [1/4] 清理旧 .o ==="
rm -f *.o

echo "=== [2/4] 重编 dropbear 全部对象（无 LX06_UNIFIED）==="
COMMONOBJS="dbutil.o buffer.o dbhelpers.o dss.o bignum.o signkey.o rsa.o dbrandom.o queue.o atomicio.o compat.o fake-rfc2553.o ltc_prng.o ecc.o ecdsa.o crypto_desc.o gensignkey.o gendss.o genrsa.o"
SVROBJS="svr-kex.o svr-auth.o sshpty.o svr-authpasswd.o svr-authpubkey.o svr-authpubkeyoptions.o svr-session.o svr-service.o svr-chansession.o svr-runopts.o svr-agentfwd.o svr-main.o svr-x11fwd.o svr-tcpfwd.o svr-authpam.o"
CLISVROBJS="common-session.o packet.o common-algo.o common-kex.o common-channel.o common-chansession.o termcodes.o loginrec.o tcp-accept.o listener.o process-packet.o dh_groups.o common-runopts.o circbuffer.o curve25519-donna.o list.o netio.o"
ALL="$COMMONOBJS $SVROBJS $CLISVROBJS"
CF="$T32 -c -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE -Wundef -fno-strict-overflow -Wno-pointer-sign -O2 -std=gnu99 -I$ZINC -I$XINC -I$DB/libtomcrypt/src/headers/ -I. -I. -DDROPBEAR_SERVER -DDROPBEAR_CLIENT -DHAVE_CRYPT"
for src in $ALL; do
  c="${src%.o}.c"
  [ -e "$c" ] || { echo "MISS src $c"; continue; }
  $CC $CF -o "$src" "$c"
done
echo "dropbear objects: $(echo $ALL | wc -w)"

echo "=== [3/4] pamstub + shim（2.41 头 → 2.25 符号桥）==="
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/pamstub2.o" "$U/pamstub2.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/shim_strl.o" "$U/shim_strl.c"
$CC $T32 -c -O2 -fPIE -o "$OBJDIR/shim225.o" "$REPO/hooks/webd/shim225.c"

echo "=== [4/4] 动态链接（设备 glibc 2.25 .so + 静态 zlib/tom）==="
[ -f "$SYSROOT_LIB/libc.so.6" ] || { echo "SYSROOT_LIB $SYSROOT_LIB 无 libc.so.6"; exit 1; }
# 解释器 = stock /lib/ld-linux-armhf.so.3（真机验证，见 build_unified.sh 注释）
LDFLAGS="-pie -Wl,-z,now -Wl,-z,relro -Wl,--as-needed -L$SYSROOT_LIB -Wl,-rpath-link,$SYSROOT_LIB -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3"
SO="-l:libc.so.6 -l:libcrypt.so.1 -l:libm.so.6 -l:libpthread.so.0 -l:libutil.so.1 -l:libgcc_s.so.1"
STATIC="$DB/libtomcrypt/libtomcrypt.a $DB/libtommath/libtommath.a $U/libz.a"
$CC $T32 $LDFLAGS -o "$OUT" $ALL \
  "$OBJDIR/pamstub2.o" "$OBJDIR/shim_strl.o" "$OBJDIR/shim225.o" $STATIC $SO
echo "link ok"

echo "=== 产物 ==="
$STRIP "$OUT" 2>/dev/null || true
ls -la "$OUT"
file "$OUT" 2>/dev/null | sed 's/, statically.*//;s/, for GNU.*/ (dynamic)/'
echo "NEEDED:"; readelf -d "$OUT" | grep -E "NEEDED|RPATH|RUNPATH" | head
echo "=== 应无 webd 符号 ==="
$NM -D "$OUT" 2>/dev/null | grep -c "webd_main" || true
echo "DONE $OUT"
