#!/bin/sh
# 小爱通道指令=运行后台测试
# LX06-SETTINGS: official_command
# LX06-RUN: root
# WEBD-UNINSTALL-TIMEOUT: 5
# LX06-EXTENSION-BEGIN
# {"version":1,"title":"后台 MCP 任务","load":"load","fields":[{"name":"official_command","env":"LX06_MCP_COMMAND","label":"小爱通道词（留空关闭）","type":"text","format":"aliases","default":"运行后台测试","maxBytes":240},{"name":"endpoint","env":"LX06_MCP_ENDPOINT","label":"MCP 服务地址","type":"text","maxBytes":512},{"name":"token","env":"LX06_MCP_TOKEN","label":"访问令牌（可选）","type":"password","maxBytes":512},{"name":"tool","env":"LX06_MCP_TOOL","label":"工具名称","type":"text","maxBytes":96},{"name":"arguments","env":"LX06_MCP_ARGUMENTS","label":"工具参数（JSON）","type":"text","default":"{}","maxBytes":1024},{"name":"timeout","env":"LX06_MCP_TIMEOUT","label":"最长执行时间（秒）","type":"number","format":"integer","min":10,"max":600,"default":"180"}],"actions":[{"id":"load","label":"读取配置","fields":[]},{"id":"save","label":"保存","fields":["official_command","endpoint","token","tool","arguments","timeout"]}]}
# LX06-EXTENSION-END

set -u
umask 077
settings=/usr/lib/lx06/script-settings
keeper=/usr/bin/lx06-keeper
worker=mcp-task-result
case "${1:-}:${2:-}" in
    --web-action:load) exec "$settings" show "$0";;
    --web-action:save) exec "$settings" save "$0";;
    --uninstall:) "$keeper" stop "$worker" >/dev/null 2>&1 || :; exit 0;;
    --official-command:*)
        [ "$#" -eq 2 ] && [ -n "$2" ] || exit 0
        exec "$settings" run "$0" /bin/sh "$0" --dispatch "$2";;
    --dispatch:*)
        [ "$#" -eq 2 ] && [ -n "$2" ] || exit 0
        printf '%s\n' "${LX06_MCP_COMMAND:-}" |
            sed 's/，/,/g;s/[[:space:].!?;:]//g;s/。//g;s/！//g;s/？//g;s/；//g;s/：//g;s/、//g;s/　//g' |
            tr ',' '\n' | grep -Fxq -- "$2" || exit 0
        # The router's one-shot action exits now. Keeper supervises the independent worker.
        if ! "$keeper" start "$worker" --root --timeout 680 -- "$settings" run "$0" /bin/sh "$0" --worker; then
            printf '%s\n' '[mcp-task] 后台任务正在运行，或暂时无法启动。'
        fi
        exit 0;;
    --worker:) ;;
    *) printf '%s\n' '[mcp-task] 请在扩展中配置 MCP，再使用小爱通道词。'; exit 0;;
esac

log() { printf '[mcp-task] %s\n' "$1"; }
say_result() {
    # Let user speech and active assistant playback finish first. Do not stop either.
    for wait_step in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        /usr/lib/lx06/xiaoai/session-run busy >/dev/null 2>&1
        [ "$?" -eq 1 ] || { sleep 1; continue; }
        status=$(ubus -t 2 call mediaplayer media_control_play_status '{}' 2>/dev/null) || { sleep 1; continue; }
        [ "$(jsonfilter -s "$status" -e '@.status' 2>/dev/null)" = 0 ] || { sleep 1; continue; }
        say_request=$(
            set +u
            . /usr/share/libubox/jshn.sh
            json_init
            json_add_string text "$1"
            json_add_int play 1
            json_add_int save 0
            json_dump
        ) || break
        answer=$(ubus -t 6 call mibrain text_to_speech "$say_request" 2>/dev/null) || break
        [ "$(jsonfilter -s "$answer" -e '@.code' 2>/dev/null)" = 0 ] || break
        log '已提交小爱播报。'
        return 0
    done
    log '小爱播报未提交。'
}
failed() { log "$1"; say_result '后台任务未完成，请查看脚本日志。'; exit 0; }

endpoint=${LX06_MCP_ENDPOINT:-}
tool=${LX06_MCP_TOOL:-}
token=${LX06_MCP_TOKEN:-}
wait_seconds=${LX06_MCP_TIMEOUT:-180}
case "$endpoint" in
    *[[:space:]]*|*\"*|*\\*|*@*|*\#*) failed 'MCP 地址无效。';;
    https://?*|http://127.0.0.1:[0-9]*) ;;
    *) failed '请配置 HTTPS MCP 地址。';;
esac
case "$tool" in ''|*[!a-zA-Z0-9_.-]*) failed '请配置有效的 MCP 工具名称。';; esac
case "$token" in *[!a-zA-Z0-9._~+/=-]*) failed 'MCP 访问令牌格式无效。';; esac
case "$wait_seconds" in ''|0*|*[!0-9]*|????*) failed '最长执行时间无效。';; esac
[ "$wait_seconds" -ge 10 ] && [ "$wait_seconds" -le 600 ] || failed '最长执行时间无效。'
args=${LX06_MCP_ARGUMENTS:-'{}'}
args=$(jsonfilter -s "$args" -e '@' 2>/dev/null) || failed '工具参数需为 JSON 对象。'
case "$args" in \{*\}) ;; *) failed '工具参数需为 JSON 对象。';; esac

tmp=$(mktemp -d /tmp/lx06-mcp-worker.XXXXXX) || failed '无法准备后台任务。'
trap 'rm -f "$tmp/request" "$tmp/headers" "$tmp/body" "$tmp/rpc" "$tmp/auth"; rmdir "$tmp"' EXIT
trap 'exit 0' HUP INT TERM
if [ -n "$token" ]; then printf 'header = "Authorization: Bearer %s"\n' "$token" > "$tmp/auth"; else : > "$tmp/auth"; fi
unset LX06_MCP_TOKEN token

rpc() {
    length=$1
    shift
    printf '%s' "$1" > "$tmp/request" || return 1
    shift
    (ulimit -f 64; curl -q -fSs --connect-timeout 5 --max-time "$length" --max-filesize 32768 \
        -K "$tmp/auth" -D "$tmp/headers" -o "$tmp/body" \
        -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
        "$@" --data-binary "@$tmp/request" -- "$endpoint") 2>/dev/null
}

read_rpc() {
    # Streamable HTTP can return a JSON message or a sequence of SSE messages.
    if grep -iq '^Content-Type:.*text/event-stream' "$tmp/headers"; then
        sed -n 's/^data: *//p' "$tmp/body" | while IFS= read -r message; do
            printf '%s' "$message" > "$tmp/rpc"
            [ "$(jsonfilter -i "$tmp/rpc" -e '@.id' 2>/dev/null)" != "$1" ] || exit 0
        done
    else
        cp "$tmp/body" "$tmp/rpc" || return 1
    fi
    [ "$(jsonfilter -i "$tmp/rpc" -e '@.id' 2>/dev/null)" = "$1" ]
}

rpc 8 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"lx06-mcp-task","version":"1.0"}}}' && read_rpc 1 || failed 'MCP 握手失败。'
protocol=$(jsonfilter -i "$tmp/rpc" -e '@.result.protocolVersion' 2>/dev/null) || failed 'MCP 未返回协议版本。'
case "$protocol" in 2025-03-26|2025-06-18|2025-11-25) ;; *) failed 'MCP 协议版本不支持。';; esac
session_id=$(sed -n 's/^[Mm][Cc][Pp]-[Ss]ession-[Ii][Dd]: *//p' "$tmp/headers" | tr -d '\r' | tail -1)
case "$session_id" in *[!a-zA-Z0-9._~+/=-]*) failed 'MCP 会话标识无效。';; esac
set -- -H "MCP-Protocol-Version: $protocol"
[ -z "$session_id" ] || set -- "$@" -H "Mcp-Session-Id: $session_id"
rpc 8 '{"jsonrpc":"2.0","method":"notifications/initialized"}' "$@" || failed 'MCP 初始化失败。'
request=$(printf '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"%s","arguments":%s}}' "$tool" "$args")
log '已发起 MCP 调用，等待独立回执。'
rpc "$wait_seconds" "$request" "$@" && read_rpc 2 || failed 'MCP 调用失败或等待超时。'
[ "$(jsonfilter -i "$tmp/rpc" -e '@.result.isError' 2>/dev/null)" != true ] || failed 'MCP 工具报告失败。'
result=$(jsonfilter -i "$tmp/rpc" -e '@.result.content[@.type="text"].text' 2>/dev/null) || failed 'MCP 没有返回可播报内容。'
[ -n "$result" ] || failed 'MCP 没有返回可播报内容。'
if [ "$(printf '%s' "$result" | wc -c)" -gt 540 ]; then
    say_result '后台任务已完成，返回内容较长，请到任务服务查看结果。'
else
    say_result "$result"
fi
