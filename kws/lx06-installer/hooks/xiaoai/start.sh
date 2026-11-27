#!/bin/sh
# LX06 官方直连启动维护；pns init 烘焙被动 processed-mic preload。
# preload 只复制 DSP 输出并仲裁原生唤醒，不改绑 speech.usock。
# S62 使用独立订阅，阈值身份不匹配的词保持待校准。
# 不创建 VOIP 会话，不删除正在使用的 TTS FIFO，不恢复/暂停播放器。
X=/usr/lib/lx06/xiaoai

log() { logger -t xiaoai -p user.info "$*"; echo "[xiaoai] $*"; }

killcomm() {
    # busybox 无 pkill 依赖：按 comm 精确杀（$1=进程名 $2=信号）
    for p in /proc/[0-9]*; do
        c=$({ tr '\000' ' ' < "$p/comm" 2>/dev/null; } 2>/dev/null) || continue
        [ "$c" = "$1" ] && kill "-${2:-TERM}" "${p#/proc/}" 2>/dev/null
    done
}


# ---------- 1. tap 时代残留清理（防御性：本固件无这些组件，防旧 /data 拉起） ----------
killcomm xaudio-tap
killcomm xiaoai-mic
killcomm official-asr

# ---------- 2. 「官方小爱同学」开关默认开（只播种，不覆盖用户设置） ----------
# 原生唤醒桥读取此开关；wakeup.sh 同步控制提示音/LED 副作用。
# 缺文件默认开；仅 Web 修改开关时持久化。

# ---------- 3. 客户端兜底拉起 ----------
# S85（755 直连 init）原生拉起客户端；开机窗口（前 180s）内缺失不算故障。
# 巡检重入时若 >180s 仍缺（init 挂掉等）则直接拉。
if ! pidof mico_aivs_lab >/dev/null 2>&1; then
    UP=$(cut -d. -f1 /proc/uptime)
    [ "${UP:-0}" -gt 180 ] && {
        sh /etc/rc.common /etc/init.d/mico_aivs_lab start >/dev/null 2>&1
        log "V44: mico_aivs_lab 兜底拉起 pid=$(pidof mico_aivs_lab)"
    }
fi

# A FIFO has no meaningful file size. Unlinking every TTS FIFO on supervisor
# re-entry can break a healthy in-progress reply; lifecycle belongs to mico.

# ---------- 5. web88 槽位应用权限维护（/tmp 每次上电重建） ----------
# /tmp/mico_aivs_lab 由引擎创建(750)、/tmp/tts 由 TTS 链路创建(755)、
# /dev/snd 属 audio 组(660) → 槽位 UID 读不了日志/开不了 PCM → 统一放宽。
chmod 666 /dev/snd/* 2>/dev/null
chmod 755 /tmp/mico_aivs_lab /tmp/mico_aivs_lab/usock 2>/dev/null
mkdir -p /tmp/tts 2>/dev/null && chmod 777 /tmp/tts 2>/dev/null

log "官方直连维护完成；被动麦流/唤醒桥由 pns init 管理"
exit 0
