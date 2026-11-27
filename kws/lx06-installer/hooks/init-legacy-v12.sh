#!/bin/sh
# /data/init.sh — LX06 统一开机启动入口（对标 .173 的 init.sh 模式，各音箱同构）。
#
# 启动链（镜像内置，刷机后即生效）：
#   /etc/init.d/done(S95) -> /etc/rc.local -> /data/init.sh
#   /etc/init.d/boot(S10)  -> init-launch.sh -> /data/init.sh   （双保险）
#
# 本文件是唯一的开机启动入口，持久存放在 /data（刷机不覆盖）。
# 刷机包本身只负责 SSH 框架，不内置任何业务功能（V12 起）。以后需要开机自启的
# 程序：
#   - 批量一次性程序：放进 /data/init.d/（每个文件一个程序），下方循环逐个拉起。
#   - 常驻程序：加到下方 user_startup() 内（受每 boot 原子锁保护，不重复叠进程）。
# 业务部署脚本（如 local-door 门控）可幂等地把自己的块写进 user_startup()：
# 用注释形式的 start/end 两行标记（<< 包裹），重复执行只替换块内内容，
# 不覆盖用户手工加的其他块。

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

# ---- user_startup：每 boot 一次，后台跑，不阻塞 boot 链 ----
# 原子抢占在 user_startup 内部（mkdir once），这里不要预建标记：V10 模板曾在此
# `: >` 预建普通文件，使内部 mkdir 恒失败。双触发（S10 + S95）由内部 mkdir 串行化。
user_startup >/tmp/lx06-user-startup.log 2>&1 &

exit 0
