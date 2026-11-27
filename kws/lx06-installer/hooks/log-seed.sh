#!/bin/sh
# /usr/lib/lx06/log-seed.sh — 日志传送配置播种（S10 boot 钩子调用，独立于 /data/init.sh）。
#
# 来源：镜像顶层 /lx06-log.conf + /lx06-log-seq（刷机期由一键刷机包
# append 进 system0）：
#   /lx06-log.conf:
#     log_server_ip = 刷机这台机器的内网 IPv4
#     log_server_port = 刷机机日志接收端口（系统推荐端口）
#     log_enabled=1（第一次刷机默认开启）
#     log_max_kb=128（滚动上限）
# 目标：/data/lx06-log.conf（看门狗重启前发送时读；WebAdmin X×5 弹窗可改）。
#
# 播种规则（独立 seq：/lx06-log-seq ↔ /data/lx06-log-seq。不与 ssh.sh 的
# /lx06-ssh-seq 混用，避免 boot 钩子内两脚本先后顺序造成检测盲区）：
#   - /data/lx06-log.conf 缺失（首刷/清 data）→ 播种
#   - 刷机 seq 变化（重新刷机）→ 重新播种（回本次刷机机器的 IP/端口）
#   - /data 里文件无效（损坏）→ 播种
#   - 其余（正常重启，seq 一致 + 有效）→ 保留用户在 Web 上改过的值
# 纯官方镜像/未带日志配置的镜像（无 /lx06-log.conf）→ 直接退出，不建文件；
# 看门狗见不到配置就不发送（fail-open，不影响 88/89 与重启路径）。

B=/lx06-log.conf
D=/data/lx06-log.conf
SEQ_BAKED=$(cat /lx06-log-seq 2>/dev/null)
SEQ_DATA=$(cat /data/lx06-log-seq 2>/dev/null)

# 校验一份完整配置：未知键/重复/缺字段/非法值 → 无效
valid_conf() {
  _f="$1"
  [ -f "$_f" ] || return 1
  _ip=0; _port=0; _en=0; _kb=0
  while IFS= read -r _line || [ -n "$_line" ]; do
    case "$_line" in
      ""|\#*) continue ;;
      *=*) _key=${_line%%=*}; _value=${_line#*=} ;;
      *) return 1 ;;
    esac
    case "$_key" in
      log_server_ip)
        [ "$_ip" = 0 ] || return 1
        _ip=1
        echo "$_value" | grep -Eq '^([0-9]{1,3}\.){3}[0-9]{1,3}$' || return 1
        # 按点拆 4 段：纯参数展开（设备 ash 下 `IFS=. set --` 前缀赋值不可靠，
        # 实测不拆分 → $#≠4 → 所有配置被判无效 → 永不播种，2026-10-20 真机 sh -x 钉死）。
        _o1=${_value%%.*}; _r=${_value#*.}
        _o2=${_r%%.*}; _r=${_r#*.}
        _o3=${_r%%.*}; _o4=${_r#*.}
        for _o in "$_o1" "$_o2" "$_o3" "$_o4"; do
          [ "$_o" -ge 0 ] 2>/dev/null && [ "$_o" -le 255 ] 2>/dev/null || return 1
        done
        ;;
      log_server_port)
        [ "$_port" = 0 ] || return 1
        _port=1
        echo "$_value" | grep -Eq '^[0-9]{1,5}$' || return 1
        [ "$_value" -ge 1 ] 2>/dev/null && [ "$_value" -le 65535 ] 2>/dev/null || return 1
        ;;
      log_enabled)
        [ "$_en" = 0 ] || return 1
        _en=1
        case "$_value" in 0|1) ;; *) return 1 ;; esac
        ;;
      log_max_kb)
        [ "$_kb" = 0 ] || return 1
        _kb=1
        echo "$_value" | grep -Eq '^[0-9]{1,5}$' || return 1
        [ "$_value" -ge 8 ] 2>/dev/null && [ "$_value" -le 1024 ] 2>/dev/null || return 1
        ;;
      *) return 1 ;;
    esac
  done < "$_f"
  [ "$_ip" = 1 ] && [ "$_port" = 1 ] && [ "$_en" = 1 ] && [ "$_kb" = 1 ] || return 1
  return 0
}

NEED=0
if [ ! -f "$B" ]; then
  # 镜像未烘焙日志配置（纯官方路径）：不播种、不覆盖
  exit 0
fi
if [ ! -f "$D" ]; then
  NEED=1
elif [ -n "$SEQ_BAKED" ] && [ "$SEQ_BAKED" != "$SEQ_DATA" ]; then
  NEED=1
elif ! valid_conf "$D"; then
  NEED=1
fi
if [ "$NEED" = 1 ]; then
  valid_conf "$B" || exit 0   # 镜像配置自身无效 → 不播种（保持无配置=不发送）
  mkdir -p /data 2>/dev/null
  cp "$B" "$D" 2>/dev/null && chmod 644 "$D" 2>/dev/null
  if [ -n "$SEQ_BAKED" ]; then
    printf '%s\n' "$SEQ_BAKED" > /data/lx06-log-seq 2>/dev/null
  else
    echo 0 > /data/lx06-log-seq 2>/dev/null
  fi
  logger -t lx06-logseed -s "seeded $D from $B" 2>/dev/null
fi
exit 0
