#!/bin/sh
# lx06-watchdog：88/89 系统级看门狗（S70，root，独立于 keeper 与用户 init.sh）。
#
# 分工（用户定案）：
#   - keeper（S55）管“单个用户程序超限”：停该程序 + 安全模式，不重启整机；
#   - 本脚本管“整机瘫痪”：88 与 89 连续 120s（12 x 10s）探活失败
#     → 原子关闭 init 开机自启动开关 → reboot -f。
#     重启后 S60 见开关已关，不加载用户代码，88/89 照常起来；
#     用户登录 WebAdmin 在脚本列表卸载问题脚本，再把开关打开。
#
# 探活语义：curl 拿到 88 的任意 HTTP 响应（含 503）= 活；nc 连上 89 = 活。
# 任一为活即活；只有两者都死的 tick 才累计，一次成功立即清零（防抖）。
# 触发重启前再探一次：已恢复则不重启，继续观察。
# busybox 1.27.2 的 timeout 语法是 `timeout -t N PROG`（GNU 式 `timeout N` 会 127 → 恒判死）。
# 本脚本自身崩溃/退出由 procd respawn 拉起（lx06-watchdog.init，respawn 30 5 5）。
# 写开关：tmp+rename+sync 原子；写与 rename 之间断电时旧文件仍完好
# （旧值缺省=on 或 off 都是安全状态，不会留下半截文件）。
# 重启前在 /data/lx06/logs/last-crash.txt 追加一行说明，WebAdmin 崩溃记录可读。
# 重启前日志传送（用户定案）：88 与 89 都死（本脚本唯一的重启触发条件）时，
#   reboot -f 之前把系统日志 POST 到 /data/lx06-log.conf 配置的日志服务端
#   （刷机期默认=刷机机内网 IP:87，WebAdmin X×5 弹窗可改；log_enabled=0 关）。
#   88/89 任一活着就不重启也不发送。发送 5s 封顶，失败不阻塞重启。
#   独立于 /data/init.sh（本脚本是 S70 系统服务），删 init.sh 不影响本功能。

FLAG=/data/lx06/autostart
LOGCONF=/data/lx06-log.conf
LIMIT=12
STEP=10

crash_note() {
  printf '%s\n' "$*" | /usr/bin/lx06-keeper log crash
}

send_crash_log() {
  # 读配置（每次重启前现读 → Web 上改完立即生效，无需重启）
  _ip=""; _port=""; _en=""; _kb=128
  if [ -f "$LOGCONF" ]; then
    _ip=$(sed -n 's/^log_server_ip=//p' "$LOGCONF" 2>/dev/null | head -1)
    _port=$(sed -n 's/^log_server_port=//p' "$LOGCONF" 2>/dev/null | head -1)
    _en=$(sed -n 's/^log_enabled=//p' "$LOGCONF" 2>/dev/null | head -1)
    _kb=$(sed -n 's/^log_max_kb=//p' "$LOGCONF" 2>/dev/null | head -1)
  fi
  [ "$_en" = "1" ] || return 0
  case "$_ip" in ""|*[!0-9.]*|*.*.*.*.*) return 0 ;; esac
  case "$_port" in ""|*[!0-9]*) return 0 ;; esac
  [ "$_kb" -ge 8 ] 2>/dev/null || _kb=128
  # 收集系统日志（128k 滚动口径）：syslog-ng 的 /var/log/messages（tmpfs）
  # + keeper 镜像 /data/lx06/logs/messages.log（持久，跨重启）
  _tmp=/tmp/lx06-crashlog.$$
  {
    echo "=== LX06 watchdog crash log $(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) ==="
    [ -f /var/log/messages ] && tail -c $((_kb * 1024)) /var/log/messages 2>/dev/null
    if [ -f /data/lx06/logs/messages.log ]; then
      echo "=== /data/lx06/logs/messages.log ==="
      tail -c $((_kb * 1024)) /data/lx06/logs/messages.log 2>/dev/null
    fi
  } > "$_tmp" 2>/dev/null
  if [ -s "$_tmp" ]; then
    crash_note "watchdog: sending crash log to ${_ip}:${_port} ($(wc -c < "$_tmp" 2>/dev/null) bytes)"
    wget -q --connect-timeout=2 --read-timeout=3 -O /dev/null --post-file="$_tmp" "http://${_ip}:${_port}/log" >/dev/null 2>&1 \
      && crash_note "watchdog: crash log sent" \
      || crash_note "watchdog: crash log send failed (timeout/网络不可达)"
  fi
  rm -f "$_tmp" 2>/dev/null
  return 0
}

bad=0
while :; do
  alive=0
  if curl -m 3 -s -o /dev/null http://127.0.0.1:88/ 2>/dev/null; then
    alive=1
  elif timeout -t 3 nc 127.0.0.1 89 </dev/null 2>/dev/null; then
    alive=1
  fi
  if [ "$alive" = 1 ]; then
    bad=0
  else
    bad=$((bad + 1))
  fi
  if [ "$bad" -ge "$LIMIT" ]; then
    if curl -m 3 -s -o /dev/null http://127.0.0.1:88/ 2>/dev/null ||
       timeout -t 3 nc 127.0.0.1 89 </dev/null 2>/dev/null; then
      bad=0
    else
      mkdir -p /data/lx06 /data/lx06/logs 2>/dev/null
      crash_note "watchdog: $(date '+%Y-%m-%d %H:%M:%S') 88/89 unreachable ${LIMIT}x${STEP}s; autostart=off; reboot -f"
      printf 'off\n' > "/data/lx06/.autostart.tmp.$$" 2>/dev/null
      mv -f "/data/lx06/.autostart.tmp.$$" "$FLAG" 2>/dev/null
      sync
      send_crash_log
      reboot -f
    fi
  fi
  sleep "$STEP"
done
