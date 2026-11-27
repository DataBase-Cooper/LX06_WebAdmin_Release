#!/bin/sh
# 用户扩展入口，不是系统服务入口。web88/ssh89 由 S50 dropbear 独立提供；
# S55 启动 keeper，S60 以 root exec 本文件（rc.local 的启动行在构建期被
# patch_user_boot.py 空操作——用户代码唯一入口 = S60 → init-launch.sh 门控）。
# 删除本文件不影响 88/89。
#
# 铁律：本文件是开机用户代码的唯一入口——看住本文件 = 看住全部开机用户代码。
#   - 「用户脚本开机自启动」总开关（/data/lx06/autostart）未打开时，
#     init-launch.sh 根本不 exec 本文件：任何用户代码（含常驻应用块）都不执行。
#   - resident 常驻应用由 web88 部署/卸载时自动增删进下方托管块（root 段），
#     不靠隐藏文件：本块列出什么，开机就只会拉起什么。
# 包含 init.d 中的 C/Go/Rust ELF 在内，全部继承非 root 身份和资源限额。
# 旧 root 安装脚本需迁移；不允许失败后降级为无限制 root 运行。

# ---- lx06-resident begin (web88 托管块：resident 应用部署/卸载时自动增删；勿手工编辑) ----
# （当前无常驻应用）
# ---- lx06-resident end ----

# live 文件 bind（必须在 root 段：本文件 exec keeper 后以 uid 61000 受限运行，
# 无 CAP_SYS_ADMIN，mount 会 EPERM 静默失败——20261022 三次重启实证：
# 放在受限段的 bind 块从未生效，/usr/lib/lx06/xiaoai 退回镜像旧 V26，
# procd 拉起镜像旧 run.sh/start.sh → 接管死循环）。
# S60 执行本段时 /data（ubi）与 /usr（squashfs）均已就绪。幂等：已挂则跳过。
# 刷机后 /data 不清。下面的 bind 会盖住镜像里的新文件。
# 镜像 stamp 变了才覆盖本包交付的文件，不删用户配置、不清 /data。
STAMP=/usr/lib/lx06/xiaoai-build-id
APPLIED=/data/lx06/.xiaoai-build-id
if [ -f "$STAMP" ] && ! cmp -s "$STAMP" "$APPLIED" 2>/dev/null; then
    mkdir -p /data/lx06/xiaoai /data/lx06/web
    # 整目录 bind 会盖住没拷进 overlay 的 run.sh/stop.sh/tapbind.so。
    for f in /usr/lib/lx06/xiaoai/*; do
        [ -f "$f" ] || continue
        cp -f "$f" /data/lx06/xiaoai/"${f##*/}"
    done
    [ -f /usr/lib/lx06/wakeup.sh ] && cp -f /usr/lib/lx06/wakeup.sh /data/lx06/wakeup.sh
    [ -f /usr/lib/lx06/web/index.html ] && cp -f /usr/lib/lx06/web/index.html /data/lx06/web/index.html
    [ -f /usr/lib/lx06/web/favicon.ico ] && cp -f /usr/lib/lx06/web/favicon.ico /data/lx06/web/favicon.ico
    [ -f /usr/lib/lx06/wakeword/ww-record.sh ] && cp -f /usr/lib/lx06/wakeword/ww-record.sh /data/local-door/ww-record.sh
    mkdir -p /data/wakeword
    [ -f /data/wakeword/official_xiaoai ] || printf '1\n' > /data/wakeword/official_xiaoai
    cp -f "$STAMP" "$APPLIED"
    chmod 755 /data/lx06/xiaoai/xiaoai-mic /data/lx06/xiaoai/vpm-feed \
        /data/lx06/xiaoai/xaudio-tap /data/lx06/xiaoai/xaudio-tap-read \
        /data/lx06/wakeup.sh 2>/dev/null
fi
[ -x /data/lx06/bin/xiaoai-mic ] && \
    mount --bind /data/lx06/bin/xiaoai-mic /usr/lib/lx06/xiaoai/xiaoai-mic 2>/dev/null
[ -x /data/lx06/bin/xaudio-tap ] && \
    mount --bind /data/lx06/bin/xaudio-tap /usr/lib/lx06/xiaoai/xaudio-tap 2>/dev/null
[ -d /data/lx06/xiaoai ] && \
    mount --bind /data/lx06/xiaoai /usr/lib/lx06/xiaoai 2>/dev/null
[ -f /data/local-door/ww-record.sh ] && \
    mount --bind /data/local-door/ww-record.sh /usr/lib/lx06/wakeword/ww-record.sh 2>/dev/null
[ -x /usr/lib/lx06/xiaoai/vpm-feed ] && ! pidof vpm-feed >/dev/null 2>&1 && \
    /sbin/start-stop-daemon -S -b -m -p /var/run/vpm-feed.pid -x /usr/lib/lx06/xiaoai/vpm-feed
[ -x /data/local-door/vpm-feed ] && ! pidof vpm-feed >/dev/null 2>&1 && \
    /sbin/start-stop-daemon -S -b -m -p /var/run/vpm-feed.pid -x /data/local-door/vpm-feed
[ -f /data/lx06/wakeup.sh ] && \
    mount --bind /data/lx06/wakeup.sh /bin/wakeup.sh 2>/dev/null
[ -f /data/lx06/wakeword/wakeword_template ] && \
    mount --bind /data/lx06/wakeword/wakeword_template /usr/lib/lx06/wakeword/wakeword_template 2>/dev/null
# V28: webd 热更（噪音抑制 agent 检测 xiaoai-mic 兼容，20261022）
[ -f /data/lx06/web/lx06webd ] && \
    mount --bind /data/lx06/web/lx06webd /usr/sbin/lx06webd 2>/dev/null

# root 段结束：手工以 root 调本文件也必须经过 keeper；真正的受限进程不会递归。
if [ "$(id -u)" = 0 ]; then
    exec /usr/bin/lx06-keeper start startup.sh --auto --timeout 0 -- /bin/sh /data/init.sh
fi

# 原子锁必须覆盖整个 init.d 循环；旧实现只锁 user_startup，导致多次启动。
# /tmp 是 tmpfs，重启清空；已有标记直接退出，不删除其他实例持有的锁。
mkdir /tmp/lx06-user-startup.once 2>/dev/null || exit 0

user_startup() {

    # 等网络/服务稳定（设备无 ping，纯 sleep）
    sleep 15

    # ---- 在这里追加你的其他常驻自启项（受同一原子锁保护）----
    # 业务块由部署脚本幂等写入（start/end 两行注释标记包裹），刷机包默认不含。
    # 例: [ -x /data/my-daemon.sh ] && { /data/my-daemon.sh >/dev/null 2>&1 & }
}

# ---- /data/init.d/*：刷机烘焙的开机自启程序（一次性任务）----
if [ -d /data/init.d ]; then
    for f in /data/init.d/*; do
        [ -e "$f" ] || continue
        case "$f" in
            *.sh) ( sh "$f" </dev/null >/dev/null 2>&1 & ) ;;
            *)    ( "$f" </dev/null >/dev/null 2>&1 & ) ;;
        esac
    done
fi

# 后台启动不阻塞；输出走 keeper 受限管道，不另建无限增长的启动日志。
user_startup &

# live 文件 bind 在文件头部 root 段执行（受限段 mount 会 EPERM，勿在此加 mount）。

exit 0
