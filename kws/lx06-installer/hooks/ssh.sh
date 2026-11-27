#!/bin/sh
# /usr/lib/lx06/ssh.sh — LX06 one-click flasher: SSH 配置 + 装配（纯同步，无看门狗）。
#
# V17 架构（学 open-xiaoai 拆蓝牙门，SSH 与蓝牙配网互不影响）：
#   stock dropbear init 被 01-ssh.patch 改造成无条件起 dropbear
#   （删掉 work_mode=bluetooth 蓝牙门 + ssh_en/channel 门），procd 保活。
#   所以本脚本不再需要"等网络→拉起 dropbear"的看门狗，只做配置装配：
#     Part A（同步，毫秒级）：
#       A.1  seed /data/lx06-ssh.conf（刷机 seq 变化/文件缺失/损坏时；
#            恢复出厂 FORCE_DEFAULT 时强制回镜像默认 89/DB_COOPER/仅局域网）
#       A.3  dropbear 配置（端口）→ /data/etc + bind-mount
#       A.4  PAM 零运行时改动（common-auth 已在构建时切 pam_unix，oxa 同款）
#       A.5  预烘焙 host key
#       A.6  root 密码 → /data/etc/shadow + bind-mount
#       A.7  应用：运行中实例端口/密码与配置不一致 → dropbear restart；
#            未运行 → start（boot 上下文跳过，由 S50 dropbear 自启）
#   触发点（全事件驱动，无轮询）：
#     1) boot() S10 钩子：SSH_AT_BOOT=1 只装配，随后 S50 dropbear 无条件起
#     2) 恢复出厂（wireless: config_clean_nosound 钩子）：写 sentinel +
#        SSH_FORCE_DEFAULT=1 调本脚本 → 当场回默认 + restart，
#        配网中 SSH 按 89/DB_COOPER/仅局域网立即可用
#     3) 用户改 /data/lx06-ssh.conf 后手动 sh /usr/lib/lx06/ssh.sh
#   LAN-only 过滤独立在 ssh-lan.sh，由 stock DHCP bound 事件触发
#   （wlan0 拿到 IP 的瞬间，每次（重）连 Wi-Fi 重算，非轮询）。
#
# 配置源：/data/lx06-ssh.conf（用户可编辑，跨重启保留）。
# 测试钩子：SSH_SH_TEST=1 跳过 A.7 的 dropbear 管理（Go 设备测试用）。

# ---- release-channel gate 标记（stock init 已删门，保留写入无害且兼容旧镜像）
mkdir -p /data 2>/dev/null
echo 1 > /data/ssh_en 2>/dev/null

LOG=/tmp/lx06-ssh-apply.log
log() { echo "[$(date)] $*" >> "$LOG" 2>/dev/null; logger -t ssh-apply -p 6 "$*" 2>/dev/null; }
: > "$LOG" 2>/dev/null

# 校验一个完整配置。未知键/重复/缺字段/端口越界/hash 非法/LAN 标志非法
# → 整份文件无效；绝不拿别的镜像的默认值拼进 /data。
PARSED_PORT=""; PARSED_HASH=""; PARSED_LAN=""
parse_ssh_conf() {
  _cfg_file="$1"
  [ -f "$_cfg_file" ] || return 1
  PARSED_PORT=""; PARSED_HASH=""; PARSED_LAN=""
  _port_seen=0; _hash_seen=0; _lan_seen=0
  while IFS= read -r _line || [ -n "$_line" ]; do
    case "$_line" in
      ""|\#*) continue ;;
      *=*) _key=${_line%%=*}; _value=${_line#*=} ;;
      *) return 1 ;;
    esac
    case "$_key" in
      ssh_port)
        [ "$_port_seen" = 0 ] || return 1
        _port_seen=1; PARSED_PORT="$_value" ;;
      ssh_root_hash)
        [ "$_hash_seen" = 0 ] || return 1
        _hash_seen=1; PARSED_HASH="$_value" ;;
      ssh_lan_only)
        [ "$_lan_seen" = 0 ] || return 1
        _lan_seen=1; PARSED_LAN="$_value" ;;
      *) return 1 ;;
    esac
  done < "$_cfg_file"
  [ "$_port_seen" = 1 ] && [ "$_hash_seen" = 1 ] && [ "$_lan_seen" = 1 ] || return 1
  case "$PARSED_PORT" in *[!0-9]*|'') return 1 ;; esac
  [ "${#PARSED_PORT}" -le 5 ] || return 1
  [ "$PARSED_PORT" -ge 1 ] 2>/dev/null && [ "$PARSED_PORT" -le 65535 ] 2>/dev/null || return 1
  printf '%s\n' "$PARSED_HASH" | grep -Eq '^\$1\$[./A-Za-z0-9]{1,8}\$[./A-Za-z0-9]{22}$' || return 1
  case "$PARSED_LAN" in 0|1) ;; *) return 1 ;; esac
  return 0
}

# 解析本镜像刷机时的配置：默认在前、可选自定义在后（自定义有效才覆盖）。
BAKED_PORT=""; BAKED_HASH=""; BAKED_LAN=""; BAKED_VALID=0
for CONF in /lx06-ssh-default.conf /lx06-ssh.conf; do
  [ -f "$CONF" ] || continue
  if parse_ssh_conf "$CONF"; then
    BAKED_PORT="$PARSED_PORT"; BAKED_HASH="$PARSED_HASH"; BAKED_LAN="$PARSED_LAN"
    BAKED_VALID=1
  else
    log "ignored invalid image SSH config: $CONF"
  fi
done
if [ "$BAKED_VALID" != 1 ]; then
  log "no valid baked SSH config; refusing to apply"
  exit 1
fi

# ---- Part A.1: seed /data/lx06-ssh.conf。
# 触发重 seed 的条件（任一）：
#   - reset sentinel 存在（恢复出厂/解绑 → 回镜像默认）
#   - /data/lx06-ssh.conf 不存在
#   - 刷机 seq 变化（新固件覆盖旧配置）
#   - /data 里配置无效（损坏）
# 普通重启（seq 一致 + 配置有效）→ 保留用户手改。
#
# R2 出厂回退：米家「恢复出厂设置」= 解绑，stock 走 /bin/unbind_device.sh：
#   rm -r -f /data/* 和 /data/.* （整个 /data 清空）→ reboot -f。
#   所以出厂后 /data/lx06-ssh.conf 不存在，若不处理会从镜像里 seed 出
#   刷机时的【自定义】配置（端口/密码不对）。build_ssh_base.sh 注入两处：
#   ① unbind_device.sh 的 rm 之后、reboot 之前：touch /data/lx06-ssh-reset
#      （sentinel 在 wipe 之后写入 → 幸存；重启后 boot 钩子跑 ssh.sh 时
#      命中 → 强制 seed 镜像默认 89/DB_COOPER/仅局域网 + 消费 sentinel）
#   ② wireless config_clean_nosound（rebind/清旧配网数据，不 wipe 不重启）：
#      touch sentinel + 同步跑 ssh.sh → 当场回默认 + dropbear restart，
#      配网中 SSH 立刻可用，sentinel 留作重启兜底。
#   正常刷机（无 reset）sentinel 不存在 → 用刷机时自定义配置。
SEQ_BAKED=""; SEQ_DATA=""; NEED_SEED=0; FORCE_DEFAULT_SEED=0
SEQ_BAKED=$(cat /lx06-ssh-seq 2>/dev/null)
SEQ_DATA=$(cat /data/lx06-ssh-seq 2>/dev/null)
if [ -f /data/lx06-ssh-reset ]; then
  NEED_SEED=1; FORCE_DEFAULT_SEED=1
  log "seed: factory-reset sentinel 命中 —— 强制回退到镜像默认配置"
  rm -f /data/lx06-ssh-reset 2>/dev/null
elif [ ! -f /data/lx06-ssh.conf ]; then
  NEED_SEED=1; log "seed: /data/lx06-ssh.conf missing"
elif [ -n "$SEQ_BAKED" ] && [ "$SEQ_BAKED" != "$SEQ_DATA" ]; then
  NEED_SEED=1; log "seed: seq changed baked=$SEQ_BAKED data=$SEQ_DATA"
elif ! parse_ssh_conf /data/lx06-ssh.conf; then
  NEED_SEED=1; log "seed: invalid /data/lx06-ssh.conf; replacing from image config"
fi
if [ "$NEED_SEED" = "1" ]; then
  if [ "$FORCE_DEFAULT_SEED" = "1" ]; then
    # 恢复出厂：只用镜像默认（22/DB_COOPER/仅局域网），忽略刷机时自定义
    if parse_ssh_conf /lx06-ssh-default.conf; then
      PORT="$PARSED_PORT"; HASH="$PARSED_HASH"; LANONLY="$PARSED_LAN"
    else
      PORT="$BAKED_PORT"; HASH="$BAKED_HASH"; LANONLY="$BAKED_LAN"
    fi
    log "seed: 使用镜像默认 port=$PORT lan_only=$LANONLY"
  else
    # 正常刷机/seq 变化/配置损坏：用镜像最终有效配置（自定义优先，否则默认）
    PORT="$BAKED_PORT"; HASH="$BAKED_HASH"; LANONLY="$BAKED_LAN"
    log "seed: 使用镜像配置 port=$PORT lan_only=$LANONLY"
  fi
  {
    echo "ssh_port=$PORT"
    echo "ssh_root_hash=$HASH"
    echo "ssh_lan_only=$LANONLY"
  } > /data/lx06-ssh.conf
  if [ -n "$SEQ_BAKED" ]; then
    printf '%s\n' "$SEQ_BAKED" > /data/lx06-ssh-seq
  else
    echo 0 > /data/lx06-ssh-seq
  fi
  log "seeded /data/lx06-ssh.conf (force_default=$FORCE_DEFAULT_SEED)"
else
  PORT="$PARSED_PORT"; HASH="$PARSED_HASH"; LANONLY="$PARSED_LAN"
  log "kept valid /data SSH config (seq match or legacy image without seq)"
fi
log "effective: port=$PORT lan_only=$LANONLY hash_len=${#HASH}"

# ---- Part A.3: stage dropbear config into /data/etc (persistent) + bind-mount.
# 全量重写（不 cp+append）：uci daemon 的 commit 周期会用 boot 早期加载的旧配置
# 覆盖 bind 目标（实测 50s→75s 从 4 行截到 3 行，rsakeyfile/Port 先后丢失）。
# 全量重写保证每次 ssh.sh 跑完文件都是完整 6 行。真正的根治在 init.d/dropbear
# 硬化（build_ssh_base.sh）：-r/端口/-g/-s 全部不依赖这个 UCI 文件，
# 它被 uci 截断也不影响 dropbear 启动与认证。无 25s 自愈兜底（补丁不是根治）。
mkdir -p /data/etc/config
{
  echo "config dropbear"
  printf '\toption PasswordAuth\t\t1\n'
  printf '\toption RootPasswordAuth\t\t1\n'
  printf '\toption Port\t\t%s\n' "$PORT"
  printf "\toption BannerFile\t\t'/etc/banner'\n"
  printf "\toption rsakeyfile '/data/etc/dropbear/dropbear_rsa_host_key'\n"
} > /data/etc/config/dropbear
mount --bind /data/etc/config/dropbear /etc/config/dropbear 2>/dev/null \
  && log "bind /etc/config/dropbear OK" || log "bind /etc/config/dropbear FAILED rc=$?"
# bind 后立即回读校验（抓「bind 成功但文件被截」的竞态证据）
log "config check: $(cat /etc/config/dropbear 2>/dev/null | grep -c rsakeyfile) rsakeyfile line(s) in live config"

# ---- Part A.4: PAM（V18 起零运行时改动）。
# stock /etc/pam.d/sshd 是 `auth include common-auth`；common-auth 已在构建时
# 切到 pam_unix（build_ssh_base.sh 烘焙，oxa 02-login.patch 同款）。运行时不再
# 写 /data/etc/pam.d/$SVC + bind 覆盖 —— V17.2 的 4 行自写 service 文件被撤掉，
# 登录路径与 open-xiaoai 真机验证可用的一比一。

# ---- Part A.5: pre-baked host key. Stock keygen() generates the key in the
#      BACKGROUND (`dropbearkey ... &`) and exits — on a cold boot with no
#      key in /data, dropbear cannot start until keygen finishes. A
#      pre-baked key satisfies both keygen() and the stock config's rsakeyfile
#      from the first second. Copied into /data once (survives a system0
#      re-flash; /data is a separate partition).
mkdir -p /data/etc/dropbear
if [ ! -s /data/etc/dropbear/dropbear_rsa_host_key ] && \
   [ -s /usr/lib/lx06/keys/dropbear_rsa_host_key ]; then
  cp /usr/lib/lx06/keys/dropbear_rsa_host_key /data/etc/dropbear/ 2>/dev/null
fi
[ -s /data/etc/dropbear/dropbear_rsa_host_key ] && \
  chmod 600 /data/etc/dropbear/dropbear_rsa_host_key 2>/dev/null

# ---- Part A.6: root password: stage /data/etc/shadow + bind-mount.
# NOTE: md5-crypt hashes use the alphabet ./A-Za-z0-9 (plus the $1$ prefix),
# so the hash CAN contain '/'. Use '|' as the sed delimiter (never in the hash
# alphabet) — a '/' delimiter silently no-ops on such hashes (2026-10-07 fix).
if [ -n "$HASH" ]; then
  cp /etc/shadow /data/etc/shadow 2>/dev/null || : > /data/etc/shadow
  if grep -q "^root:" /data/etc/shadow; then
    sed -i "s|^root:.*|root:$HASH:18128:0:99999:7:::|" /data/etc/shadow
  else
    printf 'root:%s:18128:0:99999:7:::\n' "$HASH" >> /data/etc/shadow
  fi
  mount --bind /data/etc/shadow /etc/shadow 2>/dev/null \
    && log "bind /etc/shadow OK" || log "bind /etc/shadow FAILED"
else
  log "no hash in config (empty password field?)"
fi
log "ssh.sh Part A done (config + bind-mounts)"

# ---- Part A.7: 应用（幂等，无轮询）。
# 已拆门后 dropbear 由 stock S50 无条件启动；这里只在「端口变了」或
# 「没在跑」时动作，避免无谓 restart 断开已建立的 SSH 连接。
#   【V19 关键】只有【端口变化】才 restart dropbear。改密（hash 变化）
#   【不】restart：dropbear 启动时只加载自己的配置（端口/主机 key/banner），
#   密码是 PAM 每次认证现读 /etc/shadow，改密即时生效、不断已建立的连接。
#   SSH 与 HTTP(webd) 共用同一 hash 源 /data/lx06-ssh.conf 的 ssh_root_hash：
#   webd 改密同时写 conf + /data/etc/shadow（bind 到 /etc/shadow），
#   两侧下次登录都用新密码，无需重启任何服务。
#   SSH_AT_BOOT=1（boot S10 上下文）：只装配，S50 dropbear 自启，不管理。
#   SSH_SH_TEST=1（Go 设备测试）：跳过 dropbear 管理。
APPLIED_PORT=$(cat /data/.lx06-ssh-applied 2>/dev/null)
if [ "${SSH_AT_BOOT:-0}" = "1" ] || [ "${SSH_SH_TEST:-0}" = "1" ]; then
  log "ssh.sh done (stage-only, dropbear 由 stock S50 自启)"
else
  if [ -n "$APPLIED_PORT" ] && [ "$APPLIED_PORT" = "$PORT" ]; then
    if pidof dropbear >/dev/null 2>&1; then
      log "port unchanged, dropbear running —— 无操作（改密不重启 SSH）"
    else
      log "port unchanged, dropbear 未运行 → start"
      /etc/init.d/dropbear start >/dev/null 2>&1
    fi
  else
    log "port changed (applied=${APPLIED_PORT:-无} → $PORT) → dropbear restart"
    /etc/init.d/dropbear restart >/dev/null 2>&1
  fi
  printf '%s\n' "$PORT" > /data/.lx06-ssh-applied 2>/dev/null
  printf '%s\n' "$HASH" > /data/.lx06-ssh-hash 2>/dev/null
  # LAN 过滤：IP 已就绪就立即应用一次（无 IP 时交给后续 DHCP bound 事件）
  sh /usr/lib/lx06/ssh-lan.sh >/dev/null 2>&1
  log "ssh.sh done (applied port=$PORT lan_only=$LANONLY)"
fi
exit 0
