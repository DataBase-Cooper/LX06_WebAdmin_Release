#!/bin/sh
# /usr/lib/lx06/ssh-lan.sh — LAN-only 过滤器（一次性，事件驱动）。
#
# 由 stock /bin/simple_dhcp.sh 的 `bound` 分支触发（udhcpc 给 wlan0 分到 IP
# 的瞬间 —— 系统级事件，不是轮询）。读 /data/lx06-ssh.conf：
#   ssh_lan_only=1 且 wlan0 有 IP/掩码 → 应用 ssh_lan 过滤（仅本机网段可达）
#   ssh_lan_only=0 / 无 IP → 移除过滤（全网可达 / fail-open）
# 幂等：先删后建，重复调用无副作用。毫秒级，不阻塞 DHCP 事件链。
#
# 为什么放在 bound 事件而不是 boot：boot（S10）时 wlan0 还没 IP，网段未知；
# 每次（重）连 Wi-Fi 拿到新 IP 时重算一次最正确。配网态（192.168.4.x 隐藏
# AP / 10.0.0.1）下 wlan0 的"IP"是 AP 自身地址，过滤按该网段算，手机连
# 配网 AP 时同网段 → 可达，符合预期。

CFG=/data/lx06-ssh.conf
[ -f "$CFG" ] || exit 0

PORT=""; LANONLY=""
while IFS='=' read -r k v; do
  case "$k" in
    ssh_port)     PORT="$v" ;;
    ssh_lan_only) LANONLY="$v" ;;
  esac
done < "$CFG"
[ -n "$PORT" ] || PORT=89

# 找第一个有 IP 的无线网卡
ip=""; iface=""; net=""
for IF in wlan0 wlan1; do
  _ip=$(ifconfig "$IF" 2>/dev/null | sed -n 's/.*inet[[:space:]]*addr:\([0-9.]*\).*/\1/p' | head -1)
  [ -n "$_ip" ] || continue
  _mask=$(ifconfig "$IF" 2>/dev/null | sed -n 's/.*Mask:\([0-9.]*\).*/\1/p' | head -1)
  ip="$_ip"; iface="$IF"
  case "$_mask" in
    255.255.255.0) net="${ip%.*}.0/24" ;;
    255.255.0.0)   net="${ip%.*.%.*}.0.0/16" ;;
    255.0.0.0)     net="${ip%%.*}.0.0.0/8" ;;
  esac
  break
done

# 幂等清理旧规则（89 与 88 两个端口）
for _p in 89 88; do
  iptables -D INPUT -p tcp --dport "$_p" -j ssh_lan 2>/dev/null
done
iptables -F ssh_lan 2>/dev/null
iptables -X ssh_lan 2>/dev/null

if [ "$LANONLY" = "1" ] && [ -n "$net" ]; then
  if iptables -N ssh_lan 2>/dev/null; then
    iptables -A ssh_lan -s "$net" -j ACCEPT
    iptables -A ssh_lan -p tcp --dport "$PORT" -j DROP
    iptables -A ssh_lan -p tcp --dport 88 -j DROP
    iptables -I INPUT -p tcp --dport "$PORT" -j ssh_lan
    iptables -I INPUT -p tcp --dport 88 -j ssh_lan
  fi
else
  # lan_only=0 或拿不到网段 → fail-open（全网可达），保证 SSH/HTTP 可用
  :
fi
exit 0
