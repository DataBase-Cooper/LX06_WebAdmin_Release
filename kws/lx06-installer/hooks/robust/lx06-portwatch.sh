#!/bin/sh
# lx06-portwatch：88/89 端口级自愈（S65，root，独立于 keeper 与用户 init.sh）。
#
# 背景（10/18 真机钉死，Bug 3）：reboot -f 强制重启后，下次开机 88 活、89
# Connection refused，再干净重启一次双端口才恢复。根因 = 强制重启打乱
# /data(ubifs) 挂载时序，S50 dropbear 启动时读到 squashfs 原 UCI 配置
# （Port 22），89 没监听；88 是统一 ELF 内建 webd 固定端口不受影响。
#
# 分工（V23：web/ssh 两进程拆分后按端口分流）：
#   - 本脚本管“单端口死”：88 或 89 连续 30s（2 x 15s）探活失败
#     → 88 死 → /etc/init.d/lx06webd restart（独立 web 进程，procd 保活）
#     → 89 死 → /etc/init.d/dropbear restart（纯 SSH，init 重读
#     /data/lx06-ssh.conf 的 89 端口）→ 双死 → 两个都 restart。
#   - S70 lx06-watchdog 管“双端口都死 120s”→ 关自启开关 + reboot -f。
#     本脚本排在 S70 之前（30s 自愈窗口抢在 120s 重启前）。
#
# 限频：每 boot 至多 3 次 restart。计数放 /tmp（tmpfs，重启自动清零 =
# 天然 per-boot 语义，无 boot_id 判断）。
# 探活语义与 S70 一致：curl 88 拿任意 HTTP 响应（含 503）= 活；
# busybox 语法 `timeout -t 3 nc` 连上 89 = 活。
# 只记录状态翻转（活→死 / 死→活 / 每次 restart），不逐 tick 刷屏。
# 本脚本自身崩溃由 procd respawn 拉起（lx06-portwatch.init）。

COUNT=/tmp/lx06-portwatch.count
MAX=3
STEP=15
DEAD_TICKS=2   # 2 x 15s = 30s 判死

note() {
  printf 'portwatch: %s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" |
    /usr/bin/lx06-keeper log portwatch
}

count_get() {
  cat "$COUNT" 2>/dev/null | head -1
}

dead88=0
dead89=0
while :; do
  a88=0
  a89=0
  if curl -m 3 -s -o /dev/null http://127.0.0.1:88/ 2>/dev/null; then
    a88=1
  fi
  if timeout -t 3 nc 127.0.0.1 89 </dev/null 2>/dev/null; then
    a89=1
  fi

  if [ "$a88" = 1 ]; then
    [ "$dead88" = 1 ] && note "88 recovered"
    dead88=0
  else
    if [ "$dead88" = 0 ]; then note "88 down (watch start)"; fi
    dead88=$((dead88 + 1))
  fi

  if [ "$a89" = 1 ]; then
    [ "$dead89" = 1 ] && note "89 recovered"
    dead89=0
  else
    if [ "$dead89" = 0 ]; then note "89 down (watch start)"; fi
    dead89=$((dead89 + 1))
  fi

  if [ "$dead88" -ge "$DEAD_TICKS" ] || [ "$dead89" -ge "$DEAD_TICKS" ]; then
    n=$(count_get)
    case "$n" in ""|*[!0-9]*) n=0 ;; esac
    if [ "$n" -ge "$MAX" ]; then
      : # 本 boot 已用尽自愈次数，交给 S70 决定重启
    else
      n=$((n + 1))
      printf '%s\n' "$n" > "$COUNT" 2>/dev/null
      # V23 两进程拆分：按死端口分流重启（88→lx06webd，89→dropbear，双死→都重启）
      if [ "$dead88" -ge "$DEAD_TICKS" ] && [ "$dead89" -ge "$DEAD_TICKS" ]; then
        note "restart BOTH #$n/$MAX (88dead=${dead88}x${STEP}s 89dead=${dead89}x${STEP}s)"
        /etc/init.d/lx06webd restart >/dev/null 2>&1
        /etc/init.d/dropbear restart >/dev/null 2>&1
      elif [ "$dead88" -ge "$DEAD_TICKS" ]; then
        note "restart lx06webd #$n/$MAX (88dead=${dead88}x${STEP}s, 89 ok)"
        /etc/init.d/lx06webd restart >/dev/null 2>&1
      else
        note "restart dropbear #$n/$MAX (89dead=${dead89}x${STEP}s, 88 ok)"
        /etc/init.d/dropbear restart >/dev/null 2>&1
      fi
      # restart 后两端口都进入观察期（给 dropbear 拉起时间，防连环 restart）
      sleep 20
      dead88=0
      dead89=0
      a88=0; a89=0
      if curl -m 3 -s -o /dev/null http://127.0.0.1:88/ 2>/dev/null; then a88=1; fi
      if timeout -t 3 nc 127.0.0.1 89 </dev/null 2>/dev/null; then a89=1; fi
      [ "$a88" = 1 ] && note "post-restart: 88 up" || note "post-restart: 88 still down"
      [ "$a89" = 1 ] && note "post-restart: 89 up" || note "post-restart: 89 still down"
      continue
    fi
  fi
  sleep "$STEP"
done
