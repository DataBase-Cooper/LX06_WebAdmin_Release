#!/bin/sh
# S60 调用的可信装配脚本：只播种文件，不以 root 执行用户代码。
# 88/89 的启动独立于本脚本；keeper 缺失/安全模式时失败即止，不重试。
umask 022
# 在任何 cp/chown 之前拒绝链接；尤其不能由“不存在”的悬空链接绕过 -f。
[ ! -L /data ] && [ ! -L /data/lx06 ] && [ ! -L /data/init.sh ] || exit 1
for d in /data/init.d /data/lx06/apps; do
  [ -e "$d" ] || [ -L "$d" ] || continue
  [ ! -L "$d" ] && [ -d "$d" ] || exit 1
  chown 0:0 "$d" && chmod 755 "$d" || exit 1
done

# --- 2. Seed the unified startup entry point ---
# First flash: file missing -> seed. Later flashes: seed only if the file is
# byte-identical to a KNOWN factory template (current or legacy) — user
# edits survive, template upgrades propagate to existing devices.
if [ -f /lx06-init.sh ]; then
  # 目标已存在（目录/设备文件等）时不穿过写入：只在完全不存在时播种。
  if [ ! -e /data/init.sh ]; then
    cp /lx06-init.sh /data/init.sh
  elif cmp -s /lx06-init.sh /data/init.sh; then
    :
  elif [ -f /lx06-init-legacy.sh ] && cmp -s /lx06-init-legacy.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v10.sh ] && cmp -s /lx06-init-legacy-v10.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v11.sh ] && cmp -s /lx06-init-legacy-v11.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v12.sh ] && cmp -s /lx06-init-legacy-v12.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v13.sh ] && cmp -s /lx06-init-legacy-v13.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v45.sh ] && cmp -s /lx06-init-legacy-v45.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  elif [ -f /lx06-init-legacy-v14.sh ] && cmp -s /lx06-init-legacy-v14.sh /data/init.sh; then
    cp /lx06-init.sh /data/init.sh
  fi
fi
# /tmp 是 tmpfs，冷启动时文件已丢失，不能靠启动时“回迁”恢复。
# webd 必须在上传成功前持久化 /data/lx06/apps/<name>。
# 目标不是常规文件（如目录）：fail-closed 停住，不静默跳过、也不穿过写入。
[ -f /data/init.sh ] || { echo "init-launch: /data/init.sh missing or not a regular file; user code skipped" >&2; exit 1; }
chown 0:0 /data/init.sh && chmod 755 /data/init.sh || exit 1

# --- 3. Sync baked autostart programs into /data/init.d ---
if [ -d /lx06-init.d ]; then
  for f in /lx06-init.d/*; do
    [ -e "$f" ] || continue
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    mkdir -p /data/init.d || exit 1
    dst="/data/init.d/$(basename "$f")"
    [ ! -L "$dst" ] || exit 1
    cp "$f" "$dst" && chmod 755 "$dst" || exit 1
  done
fi

# --auto 的每 boot 锁归 root keeper 管；用户删 init.sh 内部锁也不能重复自启。
# timeout=0 是可信管理端显式授权驻留，不读取用户脚本的自报豁免标记。
# 热更新清理由固件做，不依赖用户脚本开关。开关关掉也要保持 data 干净。
rm -rf /data/lx06/web /data/lx06/xiaoai /data/lx06/bin
rm -f /data/lx06/wakeup.sh \
  /data/local-door/vpm-feed /data/local-door/xaudio-tap-read /data/local-door/ww-record.sh

# --- init 开机自启动总开关（/data/lx06/autostart；文件缺省 = 开） ---
# 内容恰为 "on"（去尾换行）才加载用户代码；其余任何值（off/损坏/空）= 关。
# 铁律：总开关未打开 → 开机禁止执行任何用户脚本（本文件在此 exit，
# 不 exec /data/init.sh、不拉起任何常驻应用、不动 /dev/snd 等用户环境）。
# web88 的 GET /api/autostart 与 keeper 侧看门狗写 "off\n" 都用同一语义
# （去尾换行后恰为 on 才开）→ UI 显示恒等于开机行为。
# （2026-10-19 真机教训：历史遗留 2 字节 "on" 无尾换行，旧 webd 读成关、
# 本脚本读成开 → UI 显示未开启但开机仍拉起用户脚本。）
# S70 看门狗在 88/89 连续 120s 探活失败、reboot 之前原子写入 "off\n"：
# 下次开机到此处即停，用户代码不加载，88/89 照常起；用户登录卸载问题脚本后再开。
auto_on=1
if [ -e /data/lx06/autostart ] || [ -L /data/lx06/autostart ]; then
  auto_on=0
  case "$(cat /data/lx06/autostart 2>/dev/null)" in
    on) auto_on=1 ;;
  esac
fi
if [ "$auto_on" != 1 ]; then
  echo "init-launch: autostart switch OFF (/data/lx06/autostart); user code skipped this boot" >&2
  exit 0
fi
# --- 4. 非 root 用户代码环境（root 修复，每次开机） ---
# ALSA 声卡：用户脚本（小智 TTS 播放等）要 open /dev/snd/pcmC*，ROM 默认
# root:audio 0660 且 audio 组无人 → 非 root 一律 EACCES、放不出声。
# 本机为 LAN 单租户（88/89 已 root 直连），放开全权限可接受。
chmod 0666 /dev/snd/* 2>/dev/null || true
# mico 官方识别日志目录：官方音频服务开机后才建 /tmp/mico_aivs_lab
# （750 root:root），非 root watcher 进不了目录 → 读不到 instruction.log
# → 永远收不到唤醒词。等它出现（≤120s）后放开可读；失败不影响 88/89。
( _i=0
  while [ "$_i" -lt 24 ]; do
    if [ -d /tmp/mico_aivs_lab ]; then chmod 755 /tmp/mico_aivs_lab 2>/dev/null; break; fi
    _i=$((_i + 1)); sleep 5
  done ) &
# --- 5. 用户代码唯一入口：以 root exec /data/init.sh ---
# 常驻服务名单从本脚本迁入 /data/init.sh 的托管块（web88 在 resident
# 部署/卸载时把 /data/lx06/keeper/resident 同步渲染成 init.sh root 段的
# 逐应用启动行）：开机用户代码全部声明在 init.sh 里，看住 init.sh 即可。
# keeper 管理端只收 root 客户端，托管块位于 init.sh 的 root 段（re-exec
# 进受限 job 之前）；init.sh 的非 root 受限段无法启动常驻应用，
# 用户脚本不能绕过 init.sh 形成第二条开机自启通道。
# 上方总开关 OFF 时本行不会执行：开关未打开 = 一个用户脚本都不跑（铁律）。
# 系统唤醒组件由只读固件服务启动；这里仅进入用户入口。
exec /bin/sh /data/init.sh
