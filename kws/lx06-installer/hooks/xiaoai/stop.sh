#!/bin/sh
# LX06 V44 停止序列 · root
# 官方链路（pns/mico_aivs_lab）由各自 init.d 原生管理，本脚本只清
# tap 时代残留进程。V44 无任何 bind 挂载，无需拆除/回滚。
killcomm() {
    for p in /proc/[0-9]*; do
        [ "$(tr '\000' ' ' < "$p/comm" 2>/dev/null)" = "$1" ] && kill "${p#/proc/}" 2>/dev/null
    done
}
killcomm xaudio-tap
killcomm xiaoai-mic
killcomm official-asr
exit 0
