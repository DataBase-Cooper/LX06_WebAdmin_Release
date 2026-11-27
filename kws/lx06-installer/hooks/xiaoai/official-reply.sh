#!/bin/sh
# 官方回复门控。默认不运行 = 允许官方回复出声。
# 只有脚本自己需要挡官方 TTS 时才调用，用完必须 allow。
#
#   official-reply.sh mute [豁免目录]
#   official-reply.sh allow
#
# 做法对齐 L07A 开门方案：ubus text_to_speech action=stop + 短时 pause。
# 不重启 mico_aivs_lab。重启会把 /tmp/tts 变成空管道，官方回复全部没声。
# 不放进 xiaoai-mic.c。mic 只处理麦流，不管喇叭。

FLAG=/tmp/xiaoai-agent/official-reply.mute
GUARD_PID=/tmp/xiaoai-agent/official-reply.guard.pid
EXEMPT=${2:-}

stop_tts() {
    i=0
    while [ "$i" -lt 8 ]; do
        ubus call mibrain text_to_speech '{"action":"stop"}' >/dev/null 2>&1
        /usr/bin/mphelper pause >/dev/null 2>&1
        sleep 0.4
        i=$((i + 1))
    done
}

guard() {
    end=$(( $(date +%s) + 120 ))
    while [ -f "$FLAG" ] && [ "$(date +%s)" -lt "$end" ]; do
        for p in /proc/[0-9]*; do
            cmd=$(tr '\000' ' ' < "$p/cmdline" 2>/dev/null) || continue
            case "$cmd" in
                *miplayer*) ;;
                *) continue ;;
            esac
            if [ -n "$EXEMPT" ]; then
                case "$cmd" in
                    *"$EXEMPT"*) continue ;;
                esac
            fi
            case "$cmd" in
                */tmp/tts/*) kill "${p#/proc/}" 2>/dev/null ;;
            esac
        done
        sleep 0.3
    done
    rm -f "$FLAG" "$GUARD_PID"
    /usr/bin/mphelper play >/dev/null 2>&1
}

case "$1" in
    mute)
        mkdir -p /tmp/xiaoai-agent
        printf '%s\n' "$EXEMPT" > "$FLAG"
        if [ ! -f "$GUARD_PID" ] || ! kill -0 "$(cat "$GUARD_PID" 2>/dev/null)" 2>/dev/null; then
            guard &
            echo $! > "$GUARD_PID"
        fi
        stop_tts &
        ;;
    allow|unmute|stop)
        rm -f "$FLAG"
        if [ -f "$GUARD_PID" ]; then
            kill "$(cat "$GUARD_PID" 2>/dev/null)" 2>/dev/null
            rm -f "$GUARD_PID"
        fi
        /usr/bin/mphelper play >/dev/null 2>&1
        ;;
    *)
        echo "usage: official-reply.sh mute [exempt_dir] | allow" >&2
        exit 1
        ;;
esac
