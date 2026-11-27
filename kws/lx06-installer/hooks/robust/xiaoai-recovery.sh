#!/bin/sh
# xiaoai-recovery：官方语音栈崩溃监视 + 级联恢复（S71，root，/usr/lib/lx06/）。
#
# 背景（V44 实测根因链，2026-10-26）：
#   引擎 mipns-xiaomi 由 /etc/init.d/pns 的 procd respawn(3600 5 0) 无限拉起。
#   引擎若启动即崩（如 lib 设备名补丁丢 hw 系数 → coef_ptr nil），
#   procd 每 ~5s 重拉一次 = 崩溃循环：web88 引擎状态"未运行/运行中(D)"翻转、
#   官方无声；循环期间出生的 mediaplayer/mibrain/voip/mico_aivs_lab 半死不自愈，
#   客户端还会残留 "voip status: on" 通话态继续压制 TTS。
#
# 职责（与既有看门狗分工，用户定案）：
#   - S70 lx06-watchdog 管"整机瘫痪"（88/89 全死 → 关自启 + reboot -f）；
#   - 本脚本管"语音栈瘫痪"：只动 pns/media 系/client，绝不 reboot、绝不碰 /data。
#
# 判据与动作：
#   1) 崩溃循环：150s 窗口内引擎 PID 变化 ≥3 次（无 60s 稳定期）
#      → pns stop（断 procd 无限重拉）→ 清残留通话态 → pns start
#      → 等引擎稳定且持有 pcm 播放管线 → 按序级联重启依赖
#        （mediaplayer → mibrain_service → voip → mico_aivs_lab）。
#   2) 通话态挂死：voip_helper query != idle 持续 >300s 且 voip_service 无进程
#      → hangup_peer（z8 实锤的 TTS 压制源之一）。
#   2b) 唤醒态卡死快速自愈（z6，2026-10-27）：query != idle 持续 >30s 且无
#      对话/播放活动 → wakeup_end（z1 实锤：wake 后栈 armed 挂住吞 TTS，
#      手动 wakeup_end 已验证恢复；正常对话几秒内就离开 armed，>30s=卡死）。
#      护栏：30s 内有云端对话日志写入或 mediaplayer 持有播放 pcm = 真实
#      活动，重置计时不动手。连续 3 次 wakeup_end 仍未回 idle → 升级
#      hangup_peer（防 wakeup_end 打不动的那种挂死）。
#   3) 两次恢复尝试仍不稳 → 停手并记 engine-unrecoverable（web88 显示未运行，
#      人工介入；S70 继续守 88/89，整机不重启）。
#   全程节流：一次完整级联后 600s 内不再触发。日志 /tmp/lx06-logs/xiaoai-recovery.log
#   64KiB 轮转。busybox ash 兼容，无 bashism。
#
# pcm 持有判据说明：引擎健康 = 进程稳定 + /proc/PID/fd 里有 pcmCxDxp（播放管线已起）。
# （V44 实测：pcmC0D2p 播放 + pcmC0D3c 采集；勿用 'pcm2p' 做 grep——设备节点名是 pcmC0D2p。）

ST=${ST:-/tmp/xiaoai-recovery}
ENGINE=${ENGINE:-mipns-xiaomi}   # 恢复目标（级联里检查/重启的真引擎，恒为官方引擎）
WATCH=${WATCH:-$ENGINE}          # 崩溃循环监视目标（测试时可指向模拟进程）
WIN=${WIN:-150}      # 崩溃循环判定窗口(s)
CHANGES=${CHANGES:-3}    # 窗口内 PID 变化次数阈值
STABLE=${STABLE:-60}    # 稳定期(s)：PID 不变即算稳定
PCM_WAIT=${PCM_WAIT:-120} # 引擎起后等 pcm 持有的上限(s)
VOIP_STUCK=${VOIP_STUCK:-300}
VOIP_ARMED=${VOIP_ARMED:-30}   # z6: 非 idle 且无活动判定卡死的时长(s)
THROTTLE=${THROTTLE:-600}

log() {
  printf 'xiaoai-recovery: %s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" |
    /usr/bin/lx06-keeper log recovery
}

engine_pid() { pidof "$ENGINE" 2>/dev/null | awk '{print $1}'; }

watch_pid() { pidof "$WATCH" 2>/dev/null | awk '{print $1}'; }

engine_has_pcm() {
  p=$(engine_pid); [ -n "$p" ] || return 1
  ls -l /proc/$p/fd 2>/dev/null | grep -q 'pcmC[0-9]D[0-9]p'
}

# z6 护栏：真实对话/播放活动检测。30s 内云端对话日志有写入，或
# mediaplayer 此刻持有播放 pcm（正在出声），都算真实活动，不许动手。
dialogue_active() {
  local f age mp
  # A script's cloud/player may not touch the official dialogue logs. Its
  # verified session lease still means a conversation is in progress.
  [ -x /usr/lib/lx06/xiaoai/session-run ] && /usr/lib/lx06/xiaoai/session-run busy && return 0
  for f in /tmp/mico_aivs_lab/instruction.log /tmp/mico_aivs_lab/event.log; do
    [ -f "$f" ] || continue
    age=$(( $(date +%s) - $(stat -c %Y "$f" 2>/dev/null || echo 0) ))
    [ "$age" -lt 30 ] && return 0
  done
  mp=$(pidof mediaplayer 2>/dev/null | awk '{print $1}')
  [ -n "$mp" ] && ls -l /proc/$mp/fd 2>/dev/null | grep -q 'pcmC[0-9]D[0-9]p' && return 0
  return 1
}

wait_engine_ready() {
  # 用法: wait_engine_ready <总等待秒>。成功=PID 稳定 STABLE 秒且持有 pcm。
  deadline=$(( $(date +%s) + $1 )); seen=""; seen_at=0
  while [ "$(date +%s)" -lt "$deadline" ]; do
    p=$(engine_pid)
    if [ -n "$p" ] && [ "$p" = "$seen" ]; then
      if [ $(( $(date +%s) - seen_at )) -ge $STABLE ] && engine_has_pcm; then
        return 0
      fi
    else
      seen="$p"; seen_at=$(date +%s)
    fi
    sleep 5
  done
  return 1
}

cascade() {
  log "级联恢复开始 (原因: $1)"
  /etc/init.d/pns stop >/dev/null 2>&1
  sleep 10
  # 清残留通话态（query 非 idle 才动）
  if [ -x /usr/bin/voip_helper ]; then
    vs=$(/usr/bin/voip_helper -e query 2>/dev/null)
    [ -n "$vs" ] && [ "$vs" != "idle" ] && {
      /usr/bin/voip_helper -e hangup_peer >/dev/null 2>&1
      log "清残留通话态: $vs -> hangup_peer"
    }
  fi
  ok=0
  i=0
  while [ $i -lt 2 ]; do
    /etc/init.d/pns start >/dev/null 2>&1
    if wait_engine_ready $PCM_WAIT; then ok=1; break; fi
    i=$((i+1))
    log "引擎第 $i 次尝试未就绪，重试"
    /etc/init.d/pns stop >/dev/null 2>&1
    sleep 15
  done
  if [ "$ok" != 1 ]; then
    log "engine-unrecoverable：两次尝试引擎未稳定，停手待人工介入（pns 已 stop）"
    return 1
  fi
  log "引擎就绪 (pid $(engine_pid), pcm 已持有)，重启依赖链"
  for s in mediaplayer mibrain_service voip; do
    [ -x "/etc/init.d/$s" ] && { /etc/init.d/$s restart >/dev/null 2>&1; sleep 4; }
  done
  [ -x /etc/init.d/mico_aivs_lab ] && { sleep 4; /etc/init.d/mico_aivs_lab restart >/dev/null 2>&1; }
  log "级联恢复完成"
  return 0
}

mkdir -p "$ST" 2>/dev/null
: > "$ST/pids" 2>/dev/null      # 每行一个 epoch：PID 变化时刻
last_pid=$(watch_pid)
last_cascade=0
voip_bad_since=0
armed_since=0
armed_fires=0
prev_pid="$last_pid"

while :; do
  now=$(date +%s)
  p=$(watch_pid)

  # --- 1) PID 变化记录（崩溃循环检测，监视 WATCH 目标） ---
  if [ "$p" != "$prev_pid" ]; then
    echo "$now" >> "$ST/pids"
    prev_pid="$p"
  fi
  # 剪掉窗口外旧记录
  awk -v c="$now" -v w="$WIN" '$1 > c-w' "$ST/pids" > "$ST/pids.n" 2>/dev/null && mv -f "$ST/pids.n" "$ST/pids"
  n=$(wc -l < "$ST/pids" 2>/dev/null || echo 0)

  if [ "$n" -ge "$CHANGES" ]; then
    if [ $(( now - last_cascade )) -ge "$THROTTLE" ]; then
      last_cascade=$now
      log "检测到崩溃循环：${WIN}s 内引擎 PID 变化 ${n} 次"
      : > "$ST/pids"
      if cascade "crash-loop"; then
        prev_pid=$(engine_pid)
      else
        # 未恢复：进入长冷却（1800s 后允许再试一次）
        last_cascade=$(( now + 1200 ))
        prev_pid=$(engine_pid)
      fi
    fi
  fi

  # --- 2) 通话态挂死检测 + 2b) 唤醒态卡死快速自愈（z6） ---
  if [ -x /usr/bin/voip_helper ]; then
    vs=$(/usr/bin/voip_helper -e query 2>/dev/null)
    if [ -n "$vs" ] && [ "$vs" != "idle" ]; then
      # 2b) 快通道：无对话/播放活动且非 idle >VOIP_ARMED s → wakeup_end。
      #     正常对话几秒内离开 armed；30s+ 无活动 = 卡死吞 TTS（z1 实锤）。
      if [ "$armed_since" = 0 ]; then
        armed_since=$now
      elif [ $(( now - armed_since )) -ge "$VOIP_ARMED" ]; then
        if dialogue_active; then
          armed_since=$now    # 真实活动：重置计时，绝不断对话
        else
          armed_fires=$((armed_fires + 1))
          if [ "$armed_fires" -ge 3 ]; then
            /usr/bin/voip_helper -e hangup_peer >/dev/null 2>&1
            log "唤醒态卡死 wakeup_end x$armed_fires 无效 ($vs): 升级 hangup_peer"
            armed_fires=0
          else
            /usr/bin/voip_helper -e wakeup_end >/dev/null 2>&1
            log "唤醒态卡死 ${VOIP_ARMED}s+ ($vs, 无活动): wakeup_end #$armed_fires"
          fi
          armed_since=$now
        fi
      fi
      # 慢通道（z8）：300s+ 且 voip_service 死 → hangup_peer
      if [ "$voip_bad_since" = 0 ]; then
        voip_bad_since=$now
      elif [ $(( now - voip_bad_since )) -ge "$VOIP_STUCK" ]; then
        # voip_service 进程都不在了还挂着 busy = 残留态
        if [ -z "$(pidof voip_service 2>/dev/null)" ]; then
          /usr/bin/voip_helper -e hangup_peer >/dev/null 2>&1
          log "通话态挂死 ${VOIP_STUCK}s+ 且 voip_service 不在: hangup_peer"
        fi
        voip_bad_since=$now
      fi
    else
      voip_bad_since=0
      armed_since=0
      armed_fires=0
    fi
  fi

  sleep 10
done
