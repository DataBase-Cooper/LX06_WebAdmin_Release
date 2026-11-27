#!/bin/sh
# LX06 V44 监督循环（固件原生）· procd 实例（root，前台常驻）
# V44：官方直连架构烘焙版。巡检只剩官方链路本体：
#   1. 起 start.sh（幂等；清旧代理残留、维护开关默认值和权限）
#   2. 巡检：mipns-xiaomi 缺失 / mico_aivs_lab 缺失（开机 180s 后）→ 重跑 start.sh
#   3. official-asr 复活即杀（tap 时代兜底，直连下 = 双播主因）
# 被动麦流由 pns preload 输出，S62 管理独立 KWS 订阅；不巡检旧 vpm-feed。
# 无逃生开关（旧 tap 栈已不烘焙；如需回退刷旧包）。
X=/usr/lib/lx06/xiaoai
DATA=/data/open-xiaoai
LOG=$DATA/launcher.log

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG"; }

mkdir -p "$DATA"
# 日志限长（保内存）
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 131072 ] && mv -f "$LOG" "$LOG.1"

fails=0
while :; do
    if "$X/start.sh" >>"$LOG" 2>&1; then
        fails=0
        while :; do
            e=$(pidof mipns-xiaomi 2>/dev/null)
            c=$(pidof mico_aivs_lab 2>/dev/null)
            [ -z "$e" ] && { log "BREAK: mipns-xiaomi 缺失 → 重跑 start.sh"; break; }
            # 开机窗口（前180s）：S85 才会拉 mico_aivs_lab，此间缺失不算故障
            UP=$(cut -d. -f1 /proc/uptime)
            [ -z "$c" ] && [ "${UP:-0}" -gt 180 ] && { log "BREAK: mico_aivs_lab 缺失 → 重跑 start.sh"; break; }
            oa=$(pidof official-asr 2>/dev/null)
            if [ -n "$oa" ]; then
                for p in $oa; do kill "$p" 2>/dev/null; done
                log "official-asr 复活已杀（双播主因）"
            fi
            sleep 10
        done
    else
        fails=$((fails + 1))
        log "start.sh rc!=0 (fails=$fails)"
    fi
    sleep $([ $fails -ge 3 ] && echo 30 || echo 3)
done
