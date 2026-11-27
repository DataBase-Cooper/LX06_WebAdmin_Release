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

# 程序在固件里跑。data 不放网页、xiaoai、webd 的热更新副本。
# 本文件是用户脚本唯一入口。开机自启动总开关在 init-launch.sh：
# 关掉则不 exec 本文件，用户脚本开机不自动启动。
# V45：Embedding 程序（wakeword_template）+ 全套脚本/音频固化在固件
# /usr/lib/lx06/wakeword/，零 /data 依赖、零 bind；词表/模型/录音仍是
# /data/wakeword 下的用户数据。
mkdir -p /data/wakeword
[ -f /data/wakeword/official_xiaoai ] || printf '1\n' > /data/wakeword/official_xiaoai
# 官方 /bin/wakeup.sh 是镜像旧文件。补丁在固件 /usr/lib/lx06/wakeup.sh，挂上去，不往 data 拷。
[ -f /usr/lib/lx06/wakeup.sh ] && mount --bind /usr/lib/lx06/wakeup.sh /bin/wakeup.sh 2>/dev/null
# V44：vpm-feed 不再自启（20261026 无声事故：voip 会话踢客户端重初始化 →
# 引擎 dgram 死链 → 官方回答无声）。被动取流重构完成前，任何 voip 会话持有者都不得常驻。

# 本地唤醒词守护（wakeword_template，VPM 取流需 root）：root 段晚启动，
# 45s 等 pns/引擎就绪；幂等（脚本内含 pid 检查）。/etc 只读没有 rc.d 位，
# 这是它唯一的开机入口（20261027：断电上电后本地词全靠手拉的问题）。
( sleep 45; /etc/init.d/lx06wakeword start >/dev/null 2>&1 ) &

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
