#!/bin/sh
# LX06-HOME-MCP: 1
# LX06-RUN: root

set -eu
if [ "${1:-}" != --home-mcp-action ]; then
    printf '%s\n' '米家 MCP 已上传。请在小智扩展的「智能家居控制」中选择本脚本。'
    exit 0
fi
[ "$#" -eq 4 ] && [ -n "$3" ] && [ "${#3}" -le 96 ] || exit 2

case "$2" in
    on) command_text="打开$3";;
    off) command_text="关闭$3";;
    mode)
        case "$3" in *空调) ;; *) exit 2;; esac
        case "$4" in
            cool) mode_text=制冷;;
            heat) mode_text=制热;;
            auto) mode_text=自动;;
            dry) mode_text=除湿;;
            fan) mode_text=送风;;
            *) exit 2;;
        esac
        command_text="把${3}调到${mode_text}模式";;
    light_color|light_mode)
        case "$3" in *灯|*灯光|*灯泡|*灯带) ;; *) exit 2;; esac
        case "$2:$4" in
            light_color:white) command_text="把${3}调成白色";;
            light_color:yellow) command_text="把${3}调成黄色";;
            light_mode:moonlight) command_text="把${3}切换到月光模式";;
            light_mode:highlight) command_text="把${3}切换到高亮模式";;
            *) exit 2;;
        esac;;
    brightness|temperature)
        case "$4" in [1-9]|[1-9][0-9]|100) ;; *) exit 2;; esac
        if [ "$2" = brightness ]; then
            command_text="把$3的亮度调到百分之$4"
        else
            [ "$4" -ge 16 ] && [ "$4" -le 30 ] || exit 2
            command_text="把$3的温度调到$4度"
        fi;;
    *) exit 2;;
esac

# Keep the native login and home resolution inside the original Xiaomi service.
# It acknowledges submission only, so never infer that a device changed state.
set +u
. /usr/share/libubox/jshn.sh
json_init
json_add_string nlp_text "$command_text"
json_add_string caller wakeup
json_add_int nlp 1
json_add_int nlp_execute 1
json_add_int tts 0
json_add_int tts_play 0
json_add_int tts_save 0
request=$(json_dump)
exec /bin/ubus -t 4 call mibrain ai_service "$request"
