#!/bin/sh
# /usr/lib/lx06/ssh-diag.sh — LX06 运行时诊断推送（一次性，事件驱动，零轮询）。
#
# 触发点（由 build_ssh_base.sh 注入，非本脚本自决）：
#   1) simple_dhcp.sh bound 分支 —— wlan0 刚拿到 IP 的瞬间（权威网络事件）
#   2) boot S10 钩子内 sleep 75s 后台跑（post-S50 时机，兜底 bound 已过的场景）
#
# 行为：采集 SSH/网络/防火墙运行时快照 → 用 stock wget-ssl --post-file 推给
# 【默认网关】的 9876 端口（刷机电脑上的 diag_listen 监听）。推给网关而非硬编码
# IP：网段不写死，网关必同网段。设备 INPUT 防火墙不影响 OUTPUT，89 被 DROP 也
# 能收到诊断包。失败静默（不阻塞 DHCP/boot 事件链），每次触发只发一次。
# 参数：$1 = 触发来源标记（dhcp_bound / boot / manual）

# ---- S3: 诊断回传开关。/data/lx06-ssh.conf 的 diag_push=0 → 跳过推送（页面可关）。
#      缺省/无该键 = 开（刷机排查期默认推，修完在页面关掉即可）。 ----
DP=$(sed -n 's/^diag_push=//p' /data/lx06-ssh.conf 2>/dev/null | head -1)
SRC="${1:-manual}"
F=/tmp/lx06-diag-$$
if [ "$DP" = "0" ]; then
  echo "diag off (diag_push=0) src=$SRC" >> /tmp/lx06-ssh-apply.log 2>/dev/null
  rm -f "$F" 2>/dev/null
  exit 0
fi

: > "$F" 2>/dev/null || exit 0

{
  echo "=== LX06-DIAG ==="
  echo "src=$SRC date=$(date) uptime=$(uptime 2>/dev/null)"
  echo "[wlan0] $(ifconfig wlan0 2>/dev/null | grep -E 'inet|Mask|Bcast' | head -1)"
  echo "[gw] $(route 2>/dev/null | grep default | awk '{print $2}' | head -1)"
  echo "[work_mode] $(micocfg work_mode 2>/dev/null)"
  echo "[ssid] $(micocfg ssid 2>/dev/null)"
  echo "[config_done] $([ -f /data/status/config_done ] && echo YES || echo no)"
  echo "[dropbear pid] $(pidof dropbear 2>/dev/null)"
  ps -ef 2>/dev/null | grep dropbear | grep -v grep
  echo "[dropbear conf] $(cat /data/etc/config/dropbear 2>/dev/null | tr '\n' ' ')"
  echo "[key] $(ls -la /data/etc/dropbear/ 2>/dev/null | tail -1)"
  echo "[lx06-ssh.conf] $(cat /data/lx06-ssh.conf 2>/dev/null | tr '\n' ' ')"
  echo "[applied] $(cat /data/.lx06-ssh-applied 2>/dev/null) [seq] $(cat /data/lx06-ssh-seq 2>/dev/null) [reset] $([ -f /data/lx06-ssh-reset ] && echo EXISTS || echo none)"
  echo "--- ssh-apply.log tail ---"
  tail -30 /tmp/lx06-ssh-apply.log 2>/dev/null
  echo "--- iptables -S ---"
  iptables -S 2>&1
  echo "--- iptables -S ssh_lan ---"
  iptables -S ssh_lan 2>&1
  # ---- dropbear deep-dive (post-S50): 区分「二进制崩」vs「procd/init 没拉起」----
  echo "--- live /etc/config/dropbear (post-bind) ---"
  cat /etc/config/dropbear 2>&1 | tr '\n' ' '
  echo ""
  echo "--- procd instance cmdline (实际参数: -g/-r 在不在) ---"
  DBPID=$(pidof dropbear 2>/dev/null | head -1)
  [ -n "$DBPID" ] && { echo "pid=$DBPID"; tr '\0' ' ' < /proc/$DBPID/cmdline 2>/dev/null; echo ""; } || echo "pid=NONE"
  echo "--- shadow root line (live + /data 源) ---"
  grep '^root:' /etc/shadow 2>/dev/null
  grep '^root:' /data/etc/shadow 2>/dev/null
  echo "--- passwd root line ---"
  grep '^root:' /etc/passwd 2>/dev/null
  echo "--- live /etc/pam.d/sshd (dropbear PAM service 名=sshd) ---"
  cat /etc/pam.d/sshd 2>&1
  echo "--- pam_unix.so 模块可用性 ---"
  ls -la /lib/security/pam_unix.so 2>&1
  ldd /lib/security/pam_unix.so 2>&1 | grep -iE "not found|=>" | head -6
  echo "--- PAM probe (真机 libpam 认证路径, DB_COOPER) ---"
  if [ -x /usr/lib/lx06/pamprobe ]; then
    echo "[probe service=sshd]:"
    /usr/lib/lx06/pamprobe sshd DB_COOPER 2>&1
    echo "[probe service=sshd rc=$?]"
    echo "[probe 错误密码对照 WRONGPASS]:"
    /usr/lib/lx06/pamprobe sshd WRONGPASS 2>&1
    echo "[probe 对照 rc=$?]"
  else
    echo "pamprobe missing"
  fi
  echo "--- mounts touching etc ---"
  mount 2>/dev/null | grep -E 'etc/config|etc/shadow|pam|dropbear'
  echo "--- manual dropbear start on 2223 (decisive) ---"
  /usr/sbin/dropbear -F -P /tmp/dbtest.pid -r /data/etc/dropbear/dropbear_rsa_host_key -b /etc/banner -p 2223 >/tmp/dbtest.out 2>&1 &
  DBT=$!
  sleep 2
  kill -0 "$DBT" 2>/dev/null && echo "manual-start-alive=YES" || echo "manual-start-alive=NO"
  echo "manual-start-pid=$(pidof dropbear 2>/dev/null)"
  echo "manual-start-out: $(cat /tmp/dbtest.out 2>/dev/null | head -5 | tr '\n' '|')"
  netstat -tln 2>/dev/null | grep -E ':2223 |:22 ' || echo "port-listen=none(2223/22 not listening)"
  kill "$DBT" 2>/dev/null
  echo "--- /etc/init.d/dropbear start (init path) ---"
  /etc/init.d/dropbear start 2>&1 | head -6
  sleep 2
  echo "init-start-pid=$(pidof dropbear 2>/dev/null)"
  echo "--- logread dropbear/ssh (last 15) ---"
  logread 2>/dev/null | grep -iE 'dropbear|ssh' | tail -15
  # ---- 88/89 启动日志 + 崩溃检测（用户要求：启动是否崩溃、88/89 启动日志一并回传）----
  echo "=== 88/89 STARTUP-LOGS ==="
  echo "--- 崩溃检测 (内核日志 /dev/kmsg + dmesg) ---"
  echo "[kmsg 快照, 阻塞读最多 2s]"
  { timeout 2 cat /dev/kmsg 2>/dev/null || dmesg 2>/dev/null; } | grep -aiE "dropbear|lx06|smashing|segfault|SIGSEGV|aborted|terminated|oom|killed process" | tail -25 \
    || echo "(kmsg/dmesg: 无 dropbear/崩溃相关行)"
  echo "--- ssh-auth.log (89 侧: dropbear -E stderr 落盘, 含启动/认证/崩溃) ---"
  echo "[exists=$([ -f /tmp/lx06-logs/ssh-auth.log ] && echo YES || echo NO) size=$(wc -c < /tmp/lx06-logs/ssh-auth.log 2>/dev/null || echo 0)]"
  tail -40 /tmp/lx06-logs/ssh-auth.log 2>/dev/null
  [ -f /tmp/lx06-logs/ssh-auth.log.1 ] && { echo "[rotated .1 size=$(wc -c < /tmp/lx06-logs/ssh-auth.log.1 2>/dev/null)]"; tail -20 /tmp/lx06-logs/ssh-auth.log.1; }
  echo "--- webd 启动日志 (88 侧: webd.log, 含 socket/bind/listen 错误与重拉 banner) ---"
  echo "[exists=$([ -f /tmp/lx06-logs/webd.log ] && echo YES || echo NO) size=$(wc -c < /tmp/lx06-logs/webd.log 2>/dev/null || echo 0)]"
  tail -25 /tmp/lx06-logs/webd.log 2>/dev/null
  echo "--- webd 请求日志尾部 (88 侧) ---"
  tail -15 /tmp/lx06-logs/webd-req.log 2>/dev/null || echo "(webd-req.log 无)"
  echo "--- 88/89 监听状态 (netstat + procd) ---"
  netstat -tln 2>/dev/null | grep -E ':89 |:88 ' || echo "(88/89 均无监听!)"
  for PP in $(pidof dropbear 2>/dev/null); do
    echo "[dropbear pid=$PP] cmdline: $(tr '\0' ' ' < /proc/$PP/cmdline 2>/dev/null)"
    echo "  stat(state): $(awk '{print $3}' /proc/$PP/stat 2>/dev/null)"
  done
  echo "=== END-STARTUP-LOGS ==="
  echo "--- END-OF-DIAG ---"
} >> "$F" 2>&1

# 诊断目标：优先 /lx06-diag-target（刷机电脑 IP，构建时写入），缺省回退默认网关。
DIAG_IP=$(cat /lx06-diag-target 2>/dev/null | head -1)
case "$DIAG_IP" in
  [0-9.]*|192.*|10.*|172.*) : ;;
  *) DIAG_IP=$(route 2>/dev/null | grep default | awk '{print $2}' | head -1) ;;
esac
GW="$DIAG_IP"
if [ -n "$GW" ] && [ -s "$F" ]; then
  wget -q --connect-timeout=3 --read-timeout=3 --post-file="$F" "http://$GW:9876/diag" >/dev/null 2>&1
  RC=$?
  echo "diag sent target=$GW bytes=$(wc -c < "$F") rc=$RC src=$SRC" >> /tmp/lx06-ssh-apply.log 2>/dev/null
else
  echo "diag skip (no target/empty) gw=$GW src=$SRC" >> /tmp/lx06-ssh-apply.log 2>/dev/null
fi
rm -f "$F" 2>/dev/null
exit 0
