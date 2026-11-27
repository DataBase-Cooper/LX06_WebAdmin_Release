#!/bin/sh
# /data/init.sh — LX06 统一开机启动入口（对标 .173 的 init.sh 模式，各音箱同构）。
#
# 启动链（镜像内置，刷机后即生效）：
#   /etc/init.d/done(S95) -> /etc/rc.local -> /data/init.sh
#   /etc/init.d/boot(S10)  -> init-launch.sh -> /data/init.sh   （双保险）
#
# 本文件是唯一的开机启动入口，持久存放在 /data（刷机不覆盖）。
# 以后所有需要开机自启的程序都加到本文件里：
#   - 批量一次性程序：放进 /data/init.d/（每个文件一个程序），下方循环逐个拉起。
#   - 常驻程序：加到下方 user_startup() 内（受每 boot 原子锁保护，不重复叠进程）。
#
# 本文件由镜像内置模板 /lx06-init.sh 在首次刷机时写入（已存在则不覆盖用户
# 修改）；日常直接编辑本文件，下次开机自动生效。

LOG_FILE=/tmp/lx06-init.log
log_message() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE" 2>/dev/null
}

user_startup() {
    # 每 boot 原子锁：/tmp 是 tmpfs，重启即清；mkdir 原子 -> 重复触发不叠进程。
    # V11 修复：V10 模板在尾部用 `: >` 预先把 once 标记建成普通文件，导致这里的
    # mkdir 永远失败、user_startup 直接 return（watcher 从不自动拉起）。现在尾部
    # 不再预建标记；这里先清掉可能残留的普通文件再 mkdir 抢占。
    mkdir /tmp/lx06-user-startup.once 2>/dev/null || {
        rm -f /tmp/lx06-user-startup.once 2>/dev/null
        mkdir /tmp/lx06-user-startup.once 2>/dev/null || return 0
    }
    # 等网络/服务稳定（设备无 ping，纯 sleep）
    sleep 15

    # 本地门控 watcher（打开小区门/打开玻璃门；SSH 部署到 /data/local-door/）
    [ -x /data/local-door/watch-door.sh ] && {
        log_message "start /data/local-door/watch-door.sh"
        /data/local-door/watch-door.sh >/dev/null 2>&1 &
    }
    # 门结果回退 TTS 重生（部署后存在才跑）
    [ -x /data/local-door/regen-fallback-tts.sh ] && {
        /data/local-door/regen-fallback-tts.sh >/dev/null 2>&1 &
    }

    # ---- 在这里追加你的其他常驻自启项（受同一原子锁保护）----
    # 例: [ -x /data/my-daemon.sh ] && { /data/my-daemon.sh >/dev/null 2>&1 & }
}

# ---- relay 引导（纯文件层官方 TTS 拦截，v8 架构，版本无关）----
# 官方 TTS 通道: mibrain -> /tmp/tts/tts_*.mp3 -> mediaplayer GET 8888(nano_httpd)。
# relay 关闭时 watch-door 的清扫循环把 mp3 移走 -> 404 静音。
# 开机强制 OPEN（断电在关态也不能让官方音频永远哑着）。
mkdir -p /tmp/tts_hold
echo 0 > /tmp/local-door-relay.state
rm -f /tmp/tts_hold/tts_*.mp3 2>/dev/null

# relay 自愈: 仅当 watcher 已死且 relay 还关着时才放行（防永久静音）。
(
    while :; do
        sleep 20
        [ -x /data/local-door/watch-door.sh ] || continue
        wd_pid="$(pidof watch-door.sh 2>/dev/null | awk '{print $1}')"
        [ -z "$wd_pid" ] || continue
        [ "$(cat /tmp/local-door-relay.state 2>/dev/null)" = "1" ] || continue
        echo 0 > /tmp/local-door-relay.state
        rm -f /tmp/tts_hold/tts_*.mp3 2>/dev/null
        log_message "relay self-heal: watcher dead, closed relay reset to open"
    done
) &

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

# ---- user_startup：每 boot 一次，后台跑，不阻塞 boot 链 ----
# 原子抢占在 user_startup 内部（mkdir once），这里不要预建标记：V10 模板曾在此
# `: >` 预建普通文件，使内部 mkdir 恒失败。双触发（S10 + S95）由内部 mkdir 串行化。
user_startup >/tmp/lx06-user-startup.log 2>&1 &

exit 0
