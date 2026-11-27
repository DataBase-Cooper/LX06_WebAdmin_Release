#!/bin/sh
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# Build-time merge: official stock root.squashfs + SSH hooks -> new root.squashfs.
# Runs fully inside WSL Linux fs (proper symlinks/ownership for mksquashfs).
# Args: $1=stock img (wsl path)  $2=out img (wsl path)
# Env:  DIAG_TARGET=<ip>  刷机电脑 IP —— 写入 /lx06-diag-target，ssh-diag.sh
#   优先把诊断包 POST 到该 IP 的 9876 端口（缺省才回退设备默认网关）。
#   LX06_LEGACY_LIB / OPUS_LIBRARY: explicit stock-compatible ARM ABI inputs;
#   see docs/build-model-adapter.md. LX06_LIB uses the extracted stock /lib.
set -e
BUILD_SCRIPTS=$(cd "$(dirname "$0")" && pwd)
STOCK=$1; OUT=$2
# Resolve explicit ABI inputs before this builder changes into the image tree.
: "${LX06_LEGACY_LIB:?stock 2.25 ABI directory required; see docs/build-model-adapter.md}"
: "${OPUS_LIBRARY:?stock-compatible ARM libopus file required; see docs/build-model-adapter.md}"
LX06_LEGACY_LIB=$(cd "$LX06_LEGACY_LIB" && pwd)
OPUS_LIBRARY=$(cd "$(dirname "$OPUS_LIBRARY")" && pwd)/$(basename "$OPUS_LIBRARY")
export LX06_LEGACY_LIB OPUS_LIBRARY
# 相对路径在 cd / 后会丢目录（mksquashfs 输出到 /build/... 失败）→ 转绝对
case "$STOCK" in
  /*) ;;
  *) STOCK="$PWD/$STOCK" ;;
esac
case "$OUT" in
  /*) ;;
  *) OUT="$PWD/$OUT" ;;
esac
WORK="${LX06_WORK:-/tmp}"/lx06-sshbase.$$
rm -rf "$WORK"; mkdir -p "$WORK"
unsquashfs -q -d "$WORK" "$STOCK"
# 诊断推送目标 IP（刷机电脑）：设备 INPUT 防火墙不影响 OUTPUT，即使 22 被
# DROP，诊断包（wget --post-file 到 9876）也能到达刷机电脑。
if [ -n "$DIAG_TARGET" ]; then
  printf '%s\n' "$DIAG_TARGET" > "$WORK/lx06-diag-target"
  chmod 644 "$WORK/lx06-diag-target"
fi
# ---- V17 build-time patches（学 open-xiaoai 拆蓝牙门）----
# open-xiaoai 01-ssh.patch（已验证对 1.94.13 dropbear 干跑 RC=0）：
#   - 删 dropbear_instance 的 validate return（validation 失败不再拒起）
#   - 删 enable=0 return、删 load_interfaces 的 enable 判断
#   - 删 start_service 的蓝牙门（work_mode=bluetooth → return）+ ssh_en/channel 门
#   → dropbear 无条件启动，procd 保活。配网态/恢复出厂/蓝牙门都不再挡 SSH。
# 03-ota：禁云端 OTA（防止覆盖 system0 抹掉本镜像）。
# 04-start：rc.local 末尾播种门控入口 init-launch.sh（与 S60 同一可信入口），
# 随后 patch_user_boot.py 将其空操作——用户代码唯一活动入口 = S60；
# 若未来取消空操作，铁律仍由 init-launch.sh 总开关守住。
PATCHES=$REPO/patches
cd "$WORK"
for P in "$PATCHES"/01-ssh.patch "$PATCHES"/03-ota.patch "$PATCHES"/04-start.patch; do
  echo "applying $(basename "$P")"
  patch -p1 -N < "$P" || { echo "PATCH FAILED: $(basename "$P")"; exit 1; }
done
cd /
# ---- V18: 登录路径烤进镜像（open-xiaoai 02-login.patch 同款）----
# 根因背景：真机 PAM 探针返回 21（auth 阶段取不到信息）。oxa 真机验证可用的
# 登录路径 = ① /etc/pam.d/common-auth 切到 pam_unix（注释 libmico-pam DSA 模块，
#   启用被注释的 pam_unix.so nullok_secure 行）② /etc/shadow 的 root hash 在
#   构建时直接写进镜像。两件事都在 squashfs 里 → 恢复出厂（只清 /data）后
#   SSH 仍按镜像内 DB_COOPER 可用。sshd 的 service 文件保持 stock 的
#   `auth include common-auth`，不再运行时 bind 覆盖（V17.2 的 4 行自写
#   service 文件 + bind 撤掉，一比一复刻 oxa）。
grep -q 'lx06-pam-baked' "$WORK/etc/pam.d/common-auth" || {
  python3 - "$WORK/etc/pam.d/common-auth" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
# libmico-pam.so（DSA token，不读 /etc/shadow）→ 注释
old_auth = 'auth sufficient libmico-pam.so\n'
assert old_auth in s, "common-auth: 'auth sufficient libmico-pam.so' not found"
s = s.replace(old_auth, '# auth sufficient libmico-pam.so\n', 1)
old_acct = 'account sufficient libmico-pam.so\n'
assert old_acct in s, "common-auth: 'account sufficient libmico-pam.so' not found"
s = s.replace(old_acct, '# account sufficient libmico-pam.so\n', 1)
# 启用 stock 自带的 pam_unix 行（open-xiaoai 02-login.patch 同款）
old_unix = '# auth\t[success=1 default=ignore]\tpam_unix.so nullok_secure\n'
assert old_unix in s, "common-auth: commented pam_unix line not found"
s = s.replace(old_unix, 'auth\t[success=1 default=ignore]\tpam_unix.so nullok_secure\n', 1)
# 标记行（幂等 guard）
s = '# lx06-pam-baked: libmico-pam disabled, pam_unix enabled (open-xiaoai 02-login style)\n' + s
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print("common-auth switched to pam_unix (stock pam_unix line enabled, libmico commented)")
PYEOF
}
# 镜像内 /etc/shadow 的 root hash → 默认 DB_COOPER（与 /lx06-ssh-default.conf 一致）。
# 刷机时用户自定义密码由 ssh.sh 运行时写 /data/etc/shadow + bind 覆盖（data 优先）；
# 恢复出厂 /data 清空后 bind 消失 → 回镜像内 DB_COOPER。
grep -q 'lx06def2' "$WORK/etc/shadow" || {
  sed -i 's|^root:.*|root:$1$lx06def2$JIIifmrY3cYiEGmwCqs4E.:18128:0:99999:7:::|' "$WORK/etc/shadow"
  echo "etc/shadow root hash baked to DB_COOPER (default)"
}
# ---- V22: PAM 段移除（统一二进制不链 libpam、不走 PAM 认证）----
# 旧三件套时代：common-auth 切 pam_unix + 烤 /etc/shadow + pam_lx06tty.so shim。
# 现在：/usr/sbin/dropbear = 统一二进制，认证走 svr_auth_password() 直接
#   crypt 校验 /etc/shadow（pamstub 仅满足链接），PAM 栈不再参与。
# 保留上面「common-auth 切 pam_unix」与「shadow 烤 DB_COOPER」两步（无害，
# 且 shadow 的 root hash 仍是恢复出厂后 SSH 的密码来源，必须保留）。
# /lib/security/pam_lx06tty.so 不再安装；/etc/pam.d/sshd 保持 stock。
# hook scripts (from /mnt/c/.../hooks)
mkdir -p "$WORK/usr/lib/lx06"
cp "$REPO/hooks/ssh-apply.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/ssh.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/log-seed.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/ssh-lan.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/ssh-diag.sh" "$WORK/usr/lib/lx06/"
cp "$BUILD_SCRIPTS/../hooks/dbwrap.sh" "$WORK/usr/sbin/lx06-db"
chmod 755 "$WORK/usr/sbin/lx06-db"
# PAM 探针（真机诊断 Permission denied 根因用）：直接跑 libpam 认证路径，
# 返回码 0=成功 / 7=认证失败 / 21=拿不到 shadow 认证信息。ARM32 静态自包含。
cp "$REPO/hooks/pamprobe" "$WORK/usr/lib/lx06/"
chmod 755 "$WORK/usr/lib/lx06/pamprobe" 2>/dev/null || true
arm-linux-gnueabihf-strip -s "$WORK/usr/lib/lx06/pamprobe" 2>/dev/null || true
cp "$REPO/hooks/ssh-watch.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/init-launch.sh" "$WORK/usr/lib/lx06/"
# V28.2: 「官方小爱同学」开关门控补丁（start.sh 接管时 bind mount 到 /bin/wakeup.sh）
cp "$REPO/hooks/xiaoai/wakeup.sh" "$WORK/usr/lib/lx06/wakeup.sh"
chmod 755 "$WORK/usr/lib/lx06/wakeup.sh"
cp "$REPO/hooks/xiaoai/wakeup.sh" "$WORK/bin/wakeup.sh"
chmod 755 "$WORK/bin/wakeup.sh"
chmod 755 "$WORK"/usr/lib/lx06/*.sh
# 原生扩展监管器独立于 88/89；这里只定义将来构建时的集成，不操作在线设备。
mkdir -p "$WORK/usr/bin" "$WORK/etc/init.d" "$WORK/etc/rc.d"
bash "$BUILD_SCRIPTS/build_keeper.sh" "$WORK/usr/bin/lx06-keeper"
chmod 755 "$WORK/usr/bin/lx06-keeper"
python3 "$BUILD_SCRIPTS/patch_vendor_syslog.py" "$WORK"
# keeper.c 无 dladdr/backtrace 依赖；.symtab 是 gcc 默认产物，剥掉省 ~80KB
# （mtd4 40MiB 预算，见下方 rtwpriv 注释）。keeper 是静态链接（libc.a 来自
# 交叉工具链 2.41 集；目录被换成 2.25 .so 集时会 ld: cannot find -lc，见
# build_keeper.sh 头注）。必须用 arm-linux-gnueabihf-strip（裸 strip 是 x86
# 原生 binutils，不识别 ARM ELF → "Unable to recognise the format" 实证）。
arm-linux-gnueabihf-strip -s "$WORK/usr/bin/lx06-keeper"
cp "$REPO/hooks/robust/lx06-keeper.init" "$WORK/etc/init.d/lx06-keeper"
cp "$REPO/hooks/robust/lx06-user.init" "$WORK/etc/init.d/lx06-user"
cp "$REPO/hooks/robust/health-keeper.sh" "$WORK/usr/bin/lx06-health-keeper"
chmod 755 "$WORK/etc/init.d/lx06-keeper" "$WORK/etc/init.d/lx06-user" "$WORK/usr/bin/lx06-health-keeper"
rm -f "$WORK/etc/rc.d/S60lx06-keeper"
ln -sf ../init.d/lx06-keeper "$WORK/etc/rc.d/S55lx06-keeper"
ln -sf ../init.d/lx06-user "$WORK/etc/rc.d/S60lx06-user"
# S65 88/89 端口级自愈：单端口死 30s → restart dropbear（每 boot 至多 3 次），
# 抢在 S70 120s 整机重启之前。背景：reboot -f 后 ubifs 挂载时序被打乱，
# S50 可能读到 squashfs 原 UCI（Port 22）→ 89 不监听，88（内建 webd）正常。
cp "$REPO/hooks/robust/lx06-portwatch.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/robust/lx06-portwatch.init" "$WORK/etc/init.d/lx06-portwatch"
chmod 755 "$WORK/usr/lib/lx06/lx06-portwatch.sh" "$WORK/etc/init.d/lx06-portwatch"
ln -sf ../init.d/lx06-portwatch "$WORK/etc/rc.d/S65lx06-portwatch"
# S70 88/89 系统级看门狗：独立于 keeper 与用户 init.sh；重启前先关自启动开关。
cp "$REPO/hooks/robust/lx06-watchdog.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/robust/lx06-watchdog.init" "$WORK/etc/init.d/lx06-watchdog"
chmod 755 "$WORK/usr/lib/lx06/lx06-watchdog.sh" "$WORK/etc/init.d/lx06-watchdog"
ln -sf ../init.d/lx06-watchdog "$WORK/etc/rc.d/S70lx06-watchdog"
# S71 xiaoai-recovery：官方语音栈崩溃监视+级联恢复（z8 固件修复；只动 pns/media/client，不 reboot）
cp "$REPO/hooks/robust/xiaoai-recovery.sh" "$WORK/usr/lib/lx06/"
cp "$REPO/hooks/robust/xiaoai-recovery.init" "$WORK/etc/init.d/xiaoai-recovery"
chmod 755 "$WORK/usr/lib/lx06/xiaoai-recovery.sh" "$WORK/etc/init.d/xiaoai-recovery"
ln -sf ../init.d/xiaoai-recovery "$WORK/etc/rc.d/S71xiaoai-recovery"
# ---- V23: web/ssh 两进程拆分：纯 dropbear（SSH 89）+ 独立 lx06webd（HTTP 88）----
# 架构（替代 V22 统一二进制「单 ELF 双端口 + dropbear 父进程 fork webd 子进程」）：
#   - /usr/sbin/dropbear = 纯 dropbear 2017.75（同一源码树，不带 LX06_UNIFIED，
#     无 webd fork/看门狗/实例锁；认证仍直接 crypt 校验 /etc/shadow 不走 PAM）
#     → S50 procd 保活，只管 89。
#   - /usr/sbin/lx06webd = 独立 web 进程（webd.c + main 包装）→ S65 lx06webd
#     procd 保活，只管 88。启动时自管日志（/data/lx06/logs/webd.log，64K 轮转，
#     conf log_webd=0 关）+ 单实例 flock（/var/run/lx06webd.lock，CLOEXEC）+
#     stale-88 清理（init start_pre）+ listening 日志行（就绪权威证据）。
#   - 排障便利：ssh 上直接前台跑 /usr/sbin/lx06webd（TTY 保控制台）看现场；
#     两个进程生命周期独立，kill 谁 procd 各自重拉，互不影响。
#   - 端口级自愈按分流（lx06-portwatch）：88 死 → restart lx06webd；
#     89 死 → restart dropbear；双死 → 都 restart。S70 双死 120s → reboot -f。
#   - 动态链接设备自带 glibc 2.25（对解包出的 $WORK/lib 链接，与运行库同源）；
#     dropbear 侧 zlib/tomcrypt/tommath 静态（无 time64 符号）；
#     webd 侧 MD5-crypt 内嵌（不链 libcrypt）。_TIME_BITS=32 消 64 位 time syscall。
# 页面仍在只读 squashfs（/usr/lib/lx06/web），用户数据在 /data/lx06/（恢复出厂清空）。
# 密码源 = /data/lx06-ssh.conf 的 ssh_root_hash（SSH/HTTP 共用，改密即时生效）：
#   - SSH 每次认证读 /etc/shadow（ssh.sh 把 /data/etc/shadow bind 到 /etc/shadow）
#   - HTTP 每次 /api/login 现读 /data/lx06-ssh.conf
# LAN-only 由 ssh-lan.sh 在 DHCP bound 时按 ssh_lan_only 对 89 与 88 统一过滤。
if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
  mkdir -p "$WORK/usr/bin" "$WORK/usr/sbin" "$WORK/usr/lib/lx06/web" "$WORK/etc/init.d" "$WORK/etc/rc.d"
  # 对解包出的官方镜像 $WORK/lib 链接（设备 glibc 2.25，与运行库同源）
  LX06_LIB="$WORK/lib" bash "$REPO/scripts/build_dropbear_pure.sh" "$WORK/usr/sbin/dropbear" \
    || { echo "PURE DROPBEAR BUILD FAILED"; exit 1; }
  chmod 755 "$WORK/usr/sbin/dropbear"
  LX06_BUILD_ID="${LX06_BUILD_ID:-V20261026.1}"
  LX06_FW_VERSION="${LX06_FW_VERSION:-$LX06_BUILD_ID}"
  # LX06_FW_VERSION：固件版本号，注入 webd 编译常量 + 烧入镜像。未传时用构建号，禁止落成 dev。
  LX06_LIB="$WORK/lib" LX06_FW_VERSION="$LX06_FW_VERSION" \
    bash "$REPO/scripts/build_webd_standalone.sh" "$WORK/usr/sbin/lx06webd" \
    || { echo "WEBD BUILD FAILED"; exit 1; }
  chmod 755 "$WORK/usr/sbin/lx06webd"
  # Build every generic account/audio/HTTP/settings helper from current source.
  # Provider policy stays in uploaded scripts. Incremental release builders must
  # use the same entry and explicitly audit all replacements (see build doc).
  MODEL_COMPONENTS="$WORK/.lx06-model-components"
  LX06_LIB="$WORK/lib" bash "$REPO/scripts/build_model_adapter.sh" "$MODEL_COMPONENTS" \
    || { echo "MODEL ADAPTER BUILD FAILED"; exit 1; }
  for MODEL_HELPER in model-tool model-audio model-asr model-voice model-http; do
    install -m 755 "$MODEL_COMPONENTS/$MODEL_HELPER" "$WORK/usr/bin/lx06-$MODEL_HELPER"
  done
  install -m 755 "$MODEL_COMPONENTS/script-settings" "$WORK/usr/lib/lx06/script-settings"
  # Keep native FIFO ownership outside session-run's subreaper. Account stop,
  # goodbye and official wake must mute owned playback without abandoning TTS.
  install -m 755 "$REPO/hooks/init.d/lx06-model-tts" "$WORK/etc/init.d/lx06-model-tts"
  ln -sf ../init.d/lx06-model-tts "$WORK/etc/rc.d/S66lx06-model-tts"
  rm -rf "$MODEL_COMPONENTS"
  # webd 独立服务（S65 procd 保活，与 S65lx06-portwatch 同层不同名）
  cp "$BUILD_SCRIPTS/../hooks/init.d/lx06webd" "$WORK/etc/init.d/lx06webd"
  chmod 755 "$WORK/etc/init.d/lx06webd"
  rm -f "$WORK/etc/rc.d/S65lx06webd"
  ln -sf ../init.d/lx06webd "$WORK/etc/rc.d/S65lx06webd"
  # webd 页面（GET / 直接读 WEBDIR/index.html）
  cp "$REPO/hooks/webd/index.html" "$WORK/usr/lib/lx06/web/"
  chmod 644 "$WORK/usr/lib/lx06/web/index.html"
  cp "$REPO/hooks/webd/favicon.ico" "$WORK/usr/lib/lx06/web/favicon.ico"
  chmod 644 "$WORK/usr/lib/lx06/web/favicon.ico"
  # 固件版本文件（V20261019.5）：镜像根 /lx06-firmware-version，只读 squashfs。
  # webd /api/version 优先读它 → web88 页脚动态显示当前固件版本，
  # 刷机成功与否一眼可辨（不再靠硬编码页脚猜）。
  printf '%s\n' "$LX06_FW_VERSION" > "$WORK/lx06-firmware-version"
  chmod 644 "$WORK/lx06-firmware-version"
  echo "V23 split installed: /usr/sbin/dropbear (SSH 89) + /usr/sbin/lx06webd (HTTP 88, S65 procd)"
  echo "firmware version baked: $WORK/lx06-firmware-version = $LX06_FW_VERSION"
else
  echo "arm-linux-gnueabihf-gcc not found —— 刷机包无 SSH/HTTP"
  exit 1
fi
# ---- V24: 唤醒技能（本地模板唤醒 · 直通脚本）----
# 1) libxaudio_engine.so —— V26 起保持 100% 原厂（复原官方 KWS）：
#    V14.9/V14.9.1 曾打 6 字节补丁（hw:0,3 → noop，与旁路共享 dsnoop 采集流），
#    即 localwakeup 直接修改官方 KWS 库。V26 起 localwakeup 改走 xiaoai-agent
#    降噪 tap 流（watch-ww.sh 优先 xaudio-tap-read kws），引擎回归 stock 直开
#    hw:0,3；此处只断言输入镜像为原厂库（被打过补丁则构建失败，防回归）。
# 1b) asound.conf pcm.noop dsnoop slave 保持 stock S32_LE：
#    V14.9.1 的 S32→S16 仅服务于打过补丁的引擎（AudioRecord_Init 以 S16_LE
#    开 dsnoop）；引擎复原后无 S16 直开者（旁路 mico_record/dis 均为 plug，
#    自动转换），回归 stock。同样只断言、不修改。
# 2) /usr/lib/lx06/wakeword/: 引擎（armhf 静态）+ watch/record 脚本 + 音频。
# 3) /etc/init.d/lx06wakeword + S62（procd respawn 保活；无模型时 idle 2s 轮询）。
# 4) webd 的 /api/wakeword/* 与事件分发（ww_event_poll）随 V23 编译已入包。
WW=$REPO/hooks/wakeword
if [ -f "$WORK/usr/lib/libxaudio_engine.so" ]; then
  python3 - "$WORK/usr/lib/libxaudio_engine.so" <<'PYEOF'
import sys
p = sys.argv[1]
b = open(p, 'rb').read()
n = b.count(b'hw:0,3')
assert n == 1, "libxaudio_engine.so: 'hw:0,3' 出现 %d 次 —— V26 要求原厂库（stock 恰 1 次）；若被 6 字节补丁改过（hw:0,3->noop），输入镜像错误" % n
assert b'noop\x00\x00' not in b, "libxaudio_engine.so: 检出 V14.9 6 字节补丁残留（noop\\x00\\x00），输入镜像必须为原厂"
print("libxaudio_engine.so: stock verified (hw:0,3 x1, no V14.9 patch) —— 官方 KWS 100%% 原厂")
PYEOF
else
  echo "WARNING: $WORK/usr/lib/libxaudio_engine.so 不存在 —— 跳过原厂断言（官方 KWS 引擎缺失，需检查 stock 镜像）"
fi
# 1b) asound.conf: pcm.noop dsnoop slave 断言 stock S32_LE（V26 复原官方 KWS，
#     不再打 S16 补丁；原因见上方 V24-1b 注释）。两个文件都查：DTS 批次
#     （TAS5805 + SN 前缀命中 /etc/sn_head_list）开机 bind asound.conf.dts 覆盖
#     asound.conf（.166 即此批次）。
python3 - "$WORK/etc/asound.conf" "$WORK/etc/asound.conf.dts" <<'PYEOF'
import sys
for p in sys.argv[1:]:
    s = open(p, encoding='utf-8', errors='surrogateescape').read()
    assert 'lx06-noop-s16' not in s, "%s: 检出 V14.9.1 S16 补丁残留（lx06-noop-s16），输入镜像必须为原厂 asound" % p
    assert s.count('\t    format S32_LE') == 1, "%s: 'format S32_LE' 出现 %d 次，应恰好 1 次（pcm.noop slave，stock）" % (p, s.count('\t    format S32_LE'))
    print("%s: stock verified (pcm.noop slave S32_LE, no V14.9.1 patch)" % p)
PYEOF
if [ ! -f "$WW/build/wakeword_template" ] || [ "$WW/wakeword_template.c" -nt "$WW/build/wakeword_template" ]; then
  bash "$WW/build_ww.sh" || { echo "WAKEWORD ENGINE BUILD FAILED"; exit 1; }
fi
mkdir -p "$WORK/usr/lib/lx06/wakeword"
cp "$WW/build/wakeword_template" "$WORK/usr/lib/lx06/wakeword/"
cp "$WW/watch-ww.sh" "$WW/ww-record.sh" "$WW/ww-enroll-asr.sh" "$WORK/usr/lib/lx06/wakeword/"
if [ -f "$WW/build/wake_sample_commit" ]; then
  cp "$WW/build/wake_sample_commit" "$WORK/usr/lib/lx06/wakeword/"
fi
for A in ww-prompt.mp3 ww-test.mp3 ww-beep.wav xw.mp3; do
  [ -f "$WW/$A" ] && cp "$WW/$A" "$WORK/usr/lib/lx06/wakeword/"
done
chmod 755 "$WORK/usr/lib/lx06/wakeword/"*
cp "$WW/lx06wakeword" "$WORK/etc/init.d/lx06wakeword"
chmod 755 "$WORK/etc/init.d/lx06wakeword"
# V44: S62 不自启（20261026 无声事故：检测管道 voip 会话踢客户端重初始化 →
# 引擎 dgram 死链 → 官方回答无声，人耳+硬件双证）。引擎/脚本仍全烘焙
# （清空 data 后本地唤醒词 Embedding 程序可用），由 webd 按需拉起；
# 被动取流重构（无 voip 会话）完成后再评估恢复自启。
# ln -sf ../init.d/lx06wakeword "$WORK/etc/rc.d/S62lx06wakeword"
echo "wake-word skill installed: engine baked, S62 autostart OFF (V44 official-audio guard)"
# Stock speech socket stays direct. Legacy ctl/audio remain for compatibility;
# stage_passive_wake.py below adds the native broadcast/session stack as a unit.
XIA=$REPO/hooks/xiaoai
if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
  mkdir -p "$WORK/usr/lib/lx06/xiaoai"
  # V44: official-asr 不再烘焙（直连下双播主因，start/run 只做复活即杀）
  for T in xiaoai-ctl xiaoai-audio; do
    arm-linux-gnueabihf-gcc -O2 -static -o "$WORK/usr/lib/lx06/xiaoai/$T" \
      "$XIA/$T.c" -lm || { echo "V44 $T BUILD FAILED"; exit 1; }
    arm-linux-gnueabihf-strip -s "$WORK/usr/lib/lx06/xiaoai/$T"
  done
  # V44: start/run/stop = 固件原生直连序列（无 bind、无 vpm-feed、无逃生开关）
  for F in run.sh start.sh stop.sh official-reply.sh; do
    cp "$XIA/$F" "$WORK/usr/lib/lx06/xiaoai/" || { echo "V44 COPY FAILED: $F"; exit 1; }
  done
  cp "$XIA/lx06xiaoai" "$WORK/etc/init.d/lx06xiaoai"
  chmod 755 "$WORK/etc/init.d/lx06xiaoai" "$WORK/usr/lib/lx06/xiaoai/"*
  # V44: mico_aivs_lab init 直烘 755（stock S85 存根 644 不可执行 → 客户端
  # 永远起不来，曾靠运行时 bind 修；现直接替换镜像内文件，S85 原生可用）
  cp "$XIA/mico_aivs_lab.init.v43" "$WORK/etc/init.d/mico_aivs_lab"
  chmod 755 "$WORK/etc/init.d/mico_aivs_lab"
  # ---- V29 遗留烘焙：pns 去 opus32（pcm16 fifo 所需，保留）----
  if ! grep -q 'lx06-baked' "$WORK/etc/init.d/pns" 2>/dev/null; then
    sed -i 's/[[:space:]]-r[[:space:]]opus32//g' "$WORK/etc/init.d/pns"
    sed -i '0,/^START=55/s//# lx06-baked: 已去 -r opus32（voip 会话 fifo 须 pcm16 16k，官方云实证兼容）\nSTART=55/' "$WORK/etc/init.d/pns"
  fi
  # V30: mico_aivs_lab 不再烘焙（V29 的 tapbind LD_PRELOAD 改绑已废除；
  # stock 文件 755，mico 原生绑定 /tmp/mico_aivs_lab/usock/speech.usock）。
  rm -f "$WORK/etc/rc.d/S63lx06xiaoai"
  ln -sf ../init.d/lx06xiaoai "$WORK/etc/rc.d/S54lx06xiaoai"
  echo "$LX06_BUILD_ID" > "$WORK/usr/lib/lx06/xiaoai-build-id"
  echo "V44 xiaoai installed: ctl + audio + S54 supervisor (official direct, baked init)"
fi
# The active stack is staged as one unit: passive PCM broadcast, native official
# takeover, shared session supervisor, recorder, matching feature engine and
# calibration-aware watcher. A candidate build never seeds an old threshold.
python3 "$REPO/scripts/stage_passive_wake.py" "$WORK" \
  --feature "${LX06_WAKE_FEATURE:?set embedding-int8, embedding-fp32 or mfcc explicitly}"
# 小智脚本由用户在 web88 自己上传，固件不预装、不写入 /data。
# pre-baked dropbear host key: stock keygen() runs in the background and
# exits; on a cold boot with no key in /data, dropbear cannot start until
# keygen finishes. A pre-baked key satisfies keygen() and the stock config's
# rsakeyfile from the first second. ssh.sh copies it into /data once.
KEYS=$REPO/keys
if [ -s "$KEYS/dropbear_rsa_host_key" ]; then
  mkdir -p "$WORK/usr/lib/lx06/keys"
  cp "$KEYS/dropbear_rsa_host_key" "$WORK/usr/lib/lx06/keys/"
  chmod 600 "$WORK"/usr/lib/lx06/keys/dropbear_rsa_host_key
fi
# unified boot-startup entry point template (.173 style): installed at
# /lx06-init.sh; init-launch.sh seeds it to /data/init.sh on first flash
# and /data persists across re-flash (user edits survive).
cp "$REPO/hooks/init.sh" "$WORK/lx06-init.sh"
chmod 755 "$WORK/lx06-init.sh"
# previous factory template (V9): lets init-launch.sh upgrade existing devices
# whose /data/init.sh still holds the old factory file (user edits survive)
cp "$REPO/hooks/init-legacy.sh" "$WORK/lx06-init-legacy.sh"
chmod 755 "$WORK/lx06-init-legacy.sh"
# V10 factory template (buggy once-lock): lets init-launch.sh upgrade devices
# flashed with V10 whose /data/init.sh holds that exact file (user edits survive)
cp "$REPO/hooks/init-legacy-v10.sh" "$WORK/lx06-init-legacy-v10.sh"
chmod 755 "$WORK/lx06-init-legacy-v10.sh"
# V11 factory template (door logic baked into the startup entry point): lets
# init-launch.sh upgrade V11 devices to the V12 pure-SSH template. The door
# business itself moves OUT of the image — it is deployed over SSH and re-
# injects its own block into /data/init.sh (survives re-flash via /data).
cp "$REPO/hooks/init-legacy-v11.sh" "$WORK/lx06-init-legacy-v11.sh"
chmod 755 "$WORK/lx06-init-legacy-v11.sh"
# V12/V14.2 原厂模板仅按 cmp 字节相等升级，用户编辑过的文件绝不覆盖。
cp "$REPO/hooks/init-legacy-v12.sh" "$WORK/lx06-init-legacy-v12.sh"
chmod 755 "$WORK/lx06-init-legacy-v12.sh"
# V13 原厂模板（无 resident 托管块）：让 init-launch.sh 升级 V13 刷机且
# /data/init.sh 仍是该字节的设备（webd 托管块自愈之外的模板级兜底）。
cp "$REPO/hooks/init-legacy-v13.sh" "$WORK/lx06-init-legacy-v13.sh"
chmod 755 "$WORK/lx06-init-legacy-v13.sh"
cp "$REPO/hooks/init-legacy-v45.sh" "$WORK/lx06-init-legacy-v45.sh"
chmod 755 "$WORK/lx06-init-legacy-v45.sh"
cp "$REPO/hooks/init-legacy-v14.sh" "$WORK/lx06-init-legacy-v14.sh"
chmod 755 "$WORK/lx06-init-legacy-v14.sh"
# ---- boot() tail hook: SSH 装配（同步，毫秒级）。
# 拆门后 dropbear 由 S50 无条件启动；这里只需在 S50 之前把端口/密码/PAM/key
# 装配好（SSH_AT_BOOT=1 = 只装配不管理 dropbear，避免 S10 提前拉起后被 S50
# 再 restart 一次）。恢复出厂 sentinel 在这里消费（强制回镜像默认）。
grep -q 'lx06-oneclick-hook' "$WORK/etc/init.d/boot" || {
  python3 - "$WORK/etc/init.d/boot" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
hook = ("    # lx06-oneclick-hook\n"
        "    # 开机自启动入口 /data/init.sh：/data 被清空（恢复出厂）后立即重建，\n"
        "    # 独立于 SSH 配置判断（纯官方镜像无 /lx06-ssh.conf 也要播种）。\n"
        "    if [ ! -e /data/init.sh ] && [ ! -L /data/init.sh ] && [ -f /lx06-init.sh ]; then\n"
        "      cp /lx06-init.sh /data/init.sh 2>/dev/null && chmod 755 /data/init.sh 2>/dev/null\n"
        "    fi\n"
        "    if [ -f /lx06-ssh.conf ] || [ -f /lx06-ssh-default.conf ]; then\n"
        "      export SSH_AT_BOOT=1\n"
        "      sh /usr/lib/lx06/ssh.sh\n"
        "    fi\n"
        "    # 日志传送配置播种（与 ssh.sh 同级，独立于 /data/init.sh）：\n"
        "    # /data/lx06-log.conf ← 镜像 /lx06-log.conf（刷机机 IP/端口 87）。\n"
        "    # 镜像无该配置（纯官方路径）→ 不建文件，看门狗不发送。\n"
        "    [ -f /usr/lib/lx06/log-seed.sh ] && sh /usr/lib/lx06/log-seed.sh\n"
        "    # 用户扩展改由 S60 lx06-user 请求 keeper，不在 S10 提前启动。\n"
        "    ( sleep 75; sh /usr/lib/lx06/ssh-diag.sh boot >/dev/null 2>&1 & )\n\n")
i = s.rfind('}')
s = s[:i] + hook + s[i:]
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
PYEOF
}
# 删除 stock boot / rc.local 遗留的 root 用户自启；未知语法拒绝构建。
# 不能只增加受限入口而留下原 root 旁路，否则所有限额都可被绕开。
python3 "$REPO/scripts/patch_user_boot.py" "$WORK"
# ---- init.d/dropbear 硬化（根治 uci 配置截断 → dropbear 起不来）----
# 根因（10/9 真机诊断钉死）：Mico 的 uci daemon（-c /data/etc）在 boot 期间
# 用内存里的旧配置反复重写 /data/etc/config/dropbear（rsakeyfile/Port/
# RootPasswordAuth 行先后丢失，实测 50s=4 行、75s=3 行）。uci 文件被截后：
#   - rsakeyfile 丢 → dropbear 命令不带 -r 主机密钥 → 立即退出（qemu 对照：
#     无 -r exit=1 / 带 -r 正常监听）→ procd 无限 respawn，22 永不监听。
#   - Port 丢 → append_ports 加 -p "" → dropbear 立即退出。
#   - RootPasswordAuth 丢(默认0) → 加 -g → root 密码登录被禁（DB_COOPER 登不上）。
# stock 上 dropbear 被蓝牙/ssh_en 门控关着从不启动，所以 uci 截断从未暴露；
# 学 open-xiaoai 拆门后无条件启动才暴露。根治 = 让 dropbear 启动三要素
# （主机密钥/端口/root 密码）都不依赖那个被 uci 反复重写的文件：
#   -r 硬编码；端口读 /data/lx06-ssh.conf（ssh.sh 管的普通文件，uci 不碰）；
#   -g 行删除（-g = 禁 root 密码登录，恒不加 = root 密码恒允许）。
grep -q 'lx06-db-hardened' "$WORK/etc/init.d/dropbear" || {
  python3 - "$WORK/etc/init.d/dropbear" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()

# (1) 端口：append_ports 用 UCI ${Port}（会被 uci 截成空 → -p "" → dropbear 退出）。
#     改从 /data/lx06-ssh.conf 读（ssh.sh 写的普通文件，uci 不碰），空则 22。
old_port = '\tappend_ports "${ipaddrs}" "${Port}"'
new_port = ('\t# lx06-db-hardened: 端口读 /data/lx06-ssh.conf（uci 截断 UCI Port 后\n'
            '\t# append_ports 会加 -p "" 致 dropbear 退出；此文件是 ssh.sh 写的普通文件）。\n'
            '\tLXDB_PORT=$(sed -n "s/^ssh_port=//p" /data/lx06-ssh.conf 2>/dev/null | head -1)\n'
            '\t[ -n "${LXDB_PORT}" ] || LXDB_PORT="${Port}"\n'
            '\t[ -n "${LXDB_PORT}" ] || LXDB_PORT=89\n'
            '\tappend_ports "${ipaddrs}" "${LXDB_PORT}"')
assert old_port in s, "anchor append_ports Port not found"
s = s.replace(old_port, new_port, 1)

# (2) -s：密码认证。dropbear -s = 禁用密码认证（仅公钥）。stock 逻辑
#     [ PasswordAuth -eq 0 ] && -s 依赖 UCI PasswordAuth，它会被 uci 截成
#     默认0 → 加 -s → 端口在听但 DB_COOPER 密码登不上。改恒不加 -s。
old_s = '\t[ "${PasswordAuth}" -eq 0 ] && procd_append_param command -s\n'
assert old_s in s, "anchor -s line not found"
s = s.replace(old_s,
              ('\t# lx06-db-hardened: -s 行删除（UCI PasswordAuth 会被 uci 截成默认0\n'
               '\t# → 加 -s 禁掉密码认证；恒不加 = 密码认证恒允许）\n'), 1)

# (2b) -T：最大认证尝试数。stock 逻辑 [ MaxAuthTries -ne 0 ] && -T N，
#     UCI 默认 MaxAuthTries=3 → procd 恒加 -T 3。但统一二进制（dropbear
#     2017.75 源码）svr-runopts.c 无 case 'T' → "Invalid option -T" →
#     打印 usage 后退出 → procd 无限 respawn，89 永不监听、88 跟着没起
#     （10/11 真机诊断回传钉死：ssh-auth.log 反复 usage dump + Invalid -T）。
#     行删除 = 恒不加 -T（认证尝试数限制非必需，安全由 LAN-only 过滤兜底）。
old_t = '\t[ "${MaxAuthTries}" -ne 0 ] && procd_append_param command -T "${MaxAuthTries}"\n'
assert old_t in s, "anchor -T line not found"
s = s.replace(old_t,
              ('\t# lx06-db-hardened: -T 行删除（统一二进制 dropbear 2017.75 无\n'
               '\t# -T 选项，UCI 默认 MaxAuthTries=3 → 恒加 -T → Invalid option\n'
               '\t# → dropbear 退出 respawn，89/88 均不监听；恒不加）\n'), 1)

# (2) -g：root 密码登录开关。dropbear 2017.75 的 -g = norootpass = 禁止 root
#     密码登录（svr-runopts.c case 'g': norootpass=1；help: "-g Disable
#     password logins for root"；svr-auth.c:174 对 norootpass&&uid==0 跳过
#     svr_auth_password → root 密码恒被拒、服务端无 "Bad password" 日志）。
#     stock 逻辑 [ RootPasswordAuth -eq 0 ] && -g 依赖 UCI RootPasswordAuth，
#     它会被 uci 截成默认0 → 恒加 -g → root 密码登录被禁（DB_COOPER 登不上）。
#     改恒不加 -g = root 密码登录恒允许，不依赖 UCI。（-E 日志保留。）
old_g = '\t[ "${RootPasswordAuth}" -eq 0 ] && procd_append_param command -g\n'
assert old_g in s, "anchor -g line not found"
s = s.replace(old_g,
              ('\t# lx06-db-hardened: -g 行删除（-g = 禁 root 密码登录；恒不加 =\n'
               '\t# root 密码恒允许，不依赖会被 uci 截的 RootPasswordAuth）\n'
               '\t# lx06-s3: -E = dropbear 认证/连接/启动日志打到 stderr（wrapper\n'
               '\t# 落盘 /data/lx06/logs/ssh-auth.log，含崩溃与 "Bad password" 证据）\n'
               '\tprocd_append_param command -E\n'), 1)

# (3) -r：主机密钥。UCI rsakeyfile 被截丢 → 不带 -r → dropbear 立即退出。
#     改硬编码恒加（/data 分区里的 key，ssh.sh Part A.5 保证存在）。
old_r = '\t[ -n "${rsakeyfile}" ] && procd_append_param command -r "${rsakeyfile}"'
new_r = ('\t# lx06-db-hardened: -r 硬编码（UCI rsakeyfile 会被 uci 截丢，缺 -r 时\n'
         '\t# dropbear 立即退出；/data key 由 ssh.sh Part A.5 保证存在）。\n'
         '\tprocd_append_param command -r /data/etc/dropbear/dropbear_rsa_host_key')
assert old_r in s, "anchor -r line not found"
s = s.replace(old_r, new_r, 1)

# (4) 启动来源标记：记录本 procd 实例由 S50/boot 路径创建（boot_id+时间）。
#     供诊断区分「系统起来就启动的(S50 自启)」vs「事后被拉起」——后者说明没根治。
old_open = "\tprocd_open_instance\n"
new_open = ("\t# lx06-s3: SSH 认证日志落盘 —— dropbear -E 的 stderr 经 wrapper 追加到\n"
            "\t# /data/lx06/logs/ssh-auth.log（512KB 轮转成 .1）。本设备老版 procd 的\n"
            "\t# stdout/stderr 参数是布尔（true→/dev/null），不支持文件路径，故用\n"
            "\t# /usr/sbin/lx06-db 包装（内部 exec dropbear，PID 不变，respawn 正常）。\n"
            "\tprocd_open_instance\n"
            "\t# lx06-s3: 启动来源标记：记录本 procd 实例由 S50/boot 路径创建（boot_id+时间）。\n"
            "\t{ echo \"start-src=procd-s50 boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null) time=$(date +%s 2>/dev/null)\"; } > /tmp/lx06-db-start-src 2>/dev/null\n"
            "\tprocd_set_param command /usr/sbin/lx06-db -F -P \"$pid_file\"\n")
assert old_open in s, "anchor procd_open_instance not found"
s = s.replace(old_open, new_open, 1)

# (5) 删除 stock 的 procd_set_param command "$PROG" 行（set 语义会覆盖上面
#     刚加的 wrapper 行 → 结果 procd 直接启动 stock dropbear，wrapper 的
#     认证日志落盘失效）。wrapper 行已带 -F -P，无需 stock 行。
import re
s = re.sub(r'\n\tprocd_set_param command "\$PROG"[^\n]*', '', s, count=1)

open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print("init.d/dropbear hardened: port/-g/-r off-UCI + start-src marker + stock-command removed")
PYEOF
}
# ---- unbind_device.sh hook（恢复出厂 / 解绑 唯一入口）----
# 米家「恢复出厂设置」= 解绑：miio_helper 调 /bin/unbind_device.sh，
# 它 rm -r -f /data/* 和 /data/.*（整个 /data 清空）后 reboot -f。
# 在 wipe 之后、reboot 之前写 reset sentinel → 幸存到重启；重启后 boot 钩子
# 跑 ssh.sh 时命中 sentinel → 强制 seed 镜像默认 22/DB_COOPER/仅局域网。
# （sentinel 必须在 rm 之后写，写在 rm 前会被一起 wipe 掉。）
grep -q 'lx06-ssh-unbind-hook' "$WORK/bin/unbind_device.sh" || {
  python3 - "$WORK/bin/unbind_device.sh" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
anchor = "rm /tmp/unbind_in_process\n"
i = s.find(anchor)
if i < 0:
    raise SystemExit("unbind_device.sh: anchor 'rm /tmp/unbind_in_process' not found")
hook = ("\n"
        "# lx06-ssh-unbind-hook: 恢复出厂/解绑 → 记 reset sentinel（wipe 之后写，幸存到重启），\n"
        "#   重启后 boot 钩子跑 ssh.sh 时强制回退到镜像默认 SSH 配置（22/DB_COOPER/仅局域网）。\n"
        "touch /data/lx06-ssh-reset 2>/dev/null\n")
s = s[:i+len(anchor)] + hook + s[i+len(anchor):]
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
PYEOF
}
# ---- wireless config_clean_nosound hook（rebind/清旧配网数据，不 wipe 不重启）----
# miio_helper 在「非首次绑定（not first bind）」调它清旧配网数据（current_network_id
# 等）但不重启。这里：touch sentinel（重启兜底）+ 同步跑 ssh.sh → 当场回默认
# + dropbear restart，配网中 SSH 立刻按 22/DB_COOPER/仅局域网可用。
grep -q 'lx06-ssh-reset-hook' "$WORK/etc/init.d/wireless" || {
  python3 - "$WORK/etc/init.d/wireless" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
anchor = "config_clean_nosound()\n"
i = s.find(anchor)
if i < 0:
    raise SystemExit("config_clean_nosound() not found in wireless")
j = s.find("{", i + len(anchor))
if j < 0:
    raise SystemExit("no body for config_clean_nosound()")
hook = ("    # lx06-ssh-reset-hook: 清旧配网数据 → 回退默认 SSH 配置（配网中 SSH 立刻可用）\n"
        "    touch /data/lx06-ssh-reset 2>/dev/null\n"
        "    [ -f /usr/lib/lx06/ssh.sh ] && sh /usr/lib/lx06/ssh.sh >/dev/null 2>&1\n")
s = s[:j+1] + "\n" + hook + s[j+1:]
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
PYEOF
}
# ---- DHCP bound hook: LAN-only 过滤（事件驱动，非轮询）----
# stock 用 `udhcpc -f -S -s /bin/simple_dhcp.sh` 给 wlan0 分 IP；simple_dhcp.sh
# 的 `bound` 分支 = 「wlan0 刚拿到 IP」的权威时刻。已配网设备每次（重）连 Wi-Fi
# 都走这里 → 在此按 /data/lx06-ssh.conf 应用/移除 LAN-only iptables 过滤
# （ssh-lan.sh，幂等，毫秒级，不阻塞 DHCP 事件链）。
grep -q 'lx06-ssh-dhcp-hook' "$WORK/bin/simple_dhcp.sh" || {
  python3 - "$WORK/bin/simple_dhcp.sh" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
anchor = "        touch /tmp/simple_dhcp_bounded\n"
i = s.find(anchor)
if i < 0:
    raise SystemExit("bound anchor not found in simple_dhcp.sh")
hook = ("\n        # lx06-ssh-dhcp-hook: wlan0 拿到 IP（网络就绪）→ 应用/移除 LAN-only 过滤\n"
        "        [ -f /usr/lib/lx06/ssh-lan.sh ] && sh /usr/lib/lx06/ssh-lan.sh >/dev/null 2>&1\n"
        "        [ -f /usr/lib/lx06/ssh-diag.sh ] && ( sh /usr/lib/lx06/ssh-diag.sh dhcp_bound >/dev/null 2>&1 & )\n")
s = s[:i+len(anchor)] + hook + s[i+len(anchor):]
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
PYEOF
}
# default one-click SSH config (DB_COOPER fallback when no valid user config exists)
cat > "$WORK/lx06-ssh-default.conf" <<'CONF'
# LX06Installer default SSH config (fallback).
# Used when "官方固件 + SSH" is flashed without a valid SSH config:
#   root / port 22 / password DB_COOPER / LAN-only
# A valid flash-time /lx06-ssh.conf overrides this config in ssh.sh.
ssh_port=89
ssh_root_hash=$1$lx06def2$JIIifmrY3cYiEGmwCqs4E.
ssh_lan_only=1
CONF
chmod 644 "$WORK/lx06-ssh-default.conf"
# ---- mtd4 40MiB 尺寸预算（V20261020.5）----
# system0(mtd4) 硬上限 40MiB=41943040B。V20261020.4 镜像 41848832B，余量仅
# 94208B；xiaoai-agent 从旧 prebuilt（6514232B，启动 dlopen 不存在的
# /usr/lib/libvpm.so → 真机 FAIL-回滚循环）换成上游源码重编版（7146360B，
# 无 libvpm 引用，.166 真机验证 usock ready）→ 净增 ~632KB，必须腾出空间：
# ① 删 /usr/sbin/rtwpriv（3069904B，Realtek Wi-Fi 厂商调试工具；Wi-Fi 运行
#    靠 8821cs.ko + wpa_supplicant，stock 脚本无引用 —— grep 全树仅 .ko 内
#    的私有 ioctl 接口名，与用户态工具无关）；
# ② 上面各处 strip 自编译二进制的 .symtab（keeper/tap/tap-read/pamprobe）。
rm -f "$WORK/usr/sbin/rtwpriv"
# rebuild: same flags as stock (all-root, xz, 128k blocks, fragments, no xattr)
# CRLF 铁律：设备上跑的 shell 脚本必须是纯 LF。CRLF 行尾会让 `2>&1\r` 的 \r
# 被解析进重定向目标（ash: redir error → 脚本 rc=2 中断 → keeper
# descendant-abnormal-exit 误入安全模式，2026-10-19 .166 真机实证）。
# 烘焙前统一去 CR，覆盖所有 cp 进来的 .sh/.init 与脚本生成的文件。
CR=$(printf '\r')
find "$WORK" \( -name "*.sh" -o -name "*.init" \) -type f | while IFS= read -r f; do
    if grep -q "$CR" "$f" 2>/dev/null; then
        tr -d '\r' < "$f" > "$f.lktmp" && mv "$f.lktmp" "$f"
    fi
done
# mksquashfs APPENDS to an existing $OUT and renames conflicting dirs to _1 —
# remove it first so the image is always built from scratch.
rm -f "$OUT"
mksquashfs "$WORK" "$OUT" -comp xz -b 131072 -all-root -always-use-fragments -no-xattrs -no-exports -no-recovery
rm -rf "$WORK"
echo "REBUILT_OK $OUT"
