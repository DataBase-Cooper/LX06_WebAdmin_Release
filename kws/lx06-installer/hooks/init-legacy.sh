#!/bin/sh
# /data/init.sh — LX06 统一开机启动入口（对标 .173 的 init.sh 模式）。
#
# 这个文件是唯一的开机启动入口，持久存放在 /data（刷机不覆盖、恢复出厂前
# 保留在 /data）。以后所有需要开机自启的程序都加到本文件里：
#
#   - 单个脚本/程序：直接追加一行，例如
#       sh /data/init.d/mytool.sh </dev/null >/dev/null 2>&1 &
#   - 批量程序：保留下方 init.d 循环即可（/lx06-init.d/* 每次刷机由
#     init-launch.sh 同步到 /data/init.d/，这里逐个拉起）。
#
# 本文件由镜像内置 /data/init.sh 在首次刷机时写入（已存在则不覆盖用户
# 修改）；日常修改直接编辑本文件即可，下次开机自动生效。

# ---- /data/init.d/*：刷机烘焙的开机自启程序（heartbeat.sh 等）----
if [ -d /data/init.d ]; then
  sleep 15   # 等 Wi-Fi/DHCP 稳定（设备无 ping 命令，纯 sleep）
  for f in /data/init.d/*; do
    [ -e "$f" ] || continue
    case "$f" in
      *.sh) ( sh "$f" </dev/null >/dev/null 2>&1 & ) ;;
      *)    ( "$f" </dev/null >/dev/null 2>&1 & ) ;;
    esac
  done
fi

# ---- 在这里追加你的其他开机启动项 ----

exit 0
