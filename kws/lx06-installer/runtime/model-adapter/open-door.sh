#!/bin/sh
# 小爱通道指令=打开玻璃门,打开小区门
# LX06-SETTINGS: official_command
# LX06-RUN: root
# 官方 mibrain TTS 需要访问系统 ubus，使用固件已有的 root 运行声明。
# LX06 官方通道单次动作；由固件统一拦截此轮官方回复，无常驻进程。
# 仅收到配置的官方口令后执行。

# LX06-EXTENSION-BEGIN
# {"version":1,"title":"门禁账户","description":"填写手机号和原始密码。多个口令用逗号分隔。","load":"load","fields":[{"name":"phone","env":"LOOKDOOR_PHONE","label":"手机号","type":"text","maxBytes":20},{"name":"password","env":"LOOKDOOR_PASSWORD","label":"密码","type":"password","maxBytes":128},{"name":"official_command","env":"LOOKDOOR_OFFICIAL_COMMAND","label":"小爱通道词（留空关闭）","type":"text","format":"aliases","default":"打开玻璃门,打开小区门","maxBytes":240}],"actions":[{"id":"load","label":"读取配置","fields":[]},{"id":"save","label":"保存","fields":["phone","password","official_command"]}]}
# LX06-EXTENSION-END

set -u
umask 077
settings=/usr/lib/lx06/script-settings
case "${1:-}:${2:-}" in
    --web-action:load) exec "$settings" show "$0";;
    --web-action:save) exec "$settings" save "$0";;
    --official-command:*)
        [ "$#" -eq 2 ] && [ -n "$2" ] || exit 0
        LOOKDOOR_DISPATCH=1 exec "$settings" run "$0" /bin/sh "$0" --door-worker "$2";;
    --door-worker:*)
        [ "${LOOKDOOR_DISPATCH:-}" = 1 ] && [ "$#" -eq 2 ] && [ -n "$2" ] || exit 0
        printf '%s\n' "${LOOKDOOR_OFFICIAL_COMMAND-打开玻璃门,打开小区门}" |
            sed 's/，/,/g;s/[[:space:].!?;:]//g;s/。//g;s/！//g;s/？//g;s/；//g;s/：//g;s/、//g;s/　//g' |
            tr ',' '\n' | grep -Fxq -- "$2" || exit 0;;
    *) printf '%s\n' '[door] 已就绪，可在扩展中设置小爱通道词。'; exit 0;;
esac

LOOKDOOR_DEVICE_ID='REPLACE_WITH_DEVICE_ID'
LOOKDOOR_EQUIPMENT_ID='REPLACE_WITH_EQUIPMENT_ID'

log() { printf '[door] %s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
native_wake=$(cat /tmp/lx06-native-wake 2>/dev/null) || native_wake=''
say_result() {
    # 新的官方唤醒已经接管时，不用迟到的开门结果打断新对话。
    current_wake=$(cat /tmp/lx06-native-wake 2>/dev/null) || current_wake=''
    if [ "$native_wake" != "$current_wake" ]; then
        log '已有新的官方唤醒，结果保留在日志，本次不追加语音。'
        return
    fi
    # $1 仅来自下方固定文案，不拼接云端响应或识别文本。
    # 官方主动 TTS 使用新 dialog_id；旧官方对话的拦截记录保持不变。
    tts_reply=$(ubus -t 6 call mibrain text_to_speech \
        "{\"text\":\"$1\",\"play\":1,\"save\":0}" 2>/dev/null) || {
        log '官方 TTS 调用失败；开门结果见上方日志，不会重试开门。'
        return
    }
    tts_code=$(jsonfilter -s "$tts_reply" -e '@.code' 2>/dev/null) || tts_code=''
    if [ "$tts_code" = 0 ]; then
        log "官方 TTS 已提交：$1"
    else
        log '官方 TTS 未返回成功；开门结果见上方日志，不会重试开门。'
    fi
}
# 业务失败已记录，正常退出，避免网络故障被 keeper 当成程序崩溃。
fail() { log "$1"; say_result "${2:-开门失败，设备暂时无法执行，请查看脚本日志。}"; exit 0; }
case "${LOOKDOOR_PHONE:-}" in ''|*[!0-9]*) fail '请在扩展中填写手机号。';; esac
[ -n "${LOOKDOOR_PASSWORD:-}" ] || fail '请在扩展中填写密码。'
case "$LOOKDOOR_DEVICE_ID:$LOOKDOOR_EQUIPMENT_ID" in *REPLACE_WITH*|:|:*|*:) fail '门禁设备未配置。';; esac
for tool in curl jsonfilter mktemp md5sum awk; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少依赖：$tool；未发送开门请求。"
done
work=$(mktemp -d /tmp/lx06-door.XXXXXX) || fail '无法创建临时目录；未发送开门请求。'
cleanup() { rm -f "$work/cookie" "$work/response" "$work/phone" "$work/password-md5"; rmdir "$work" 2>/dev/null; }
trap cleanup 0
trap 'exit 0' 1 2 15

printf %s "$LOOKDOOR_PHONE" > "$work/phone" || fail '无法准备登录'
printf %s "$LOOKDOOR_PASSWORD" | md5sum | awk '{printf "%s",$1}' > "$work/password-md5" || fail '无法准备登录'
unset LOOKDOOR_PASSWORD
[ "$(wc -c < "$work/password-md5")" -eq 32 ] || fail '无法准备登录'
log '收到打开玻璃门指令，正在登录门禁服务。'
curl -fsS --connect-timeout 4 --max-time 7 --max-filesize 32768 \
    -o "$work/response" -c "$work/cookie" \
    --data-urlencode "pNn@$work/phone" \
    --data-urlencode "newDeviceId=$LOOKDOOR_DEVICE_ID" \
    --data-urlencode 'equipmentFlag=2' \
    --data-urlencode "pWd@$work/password-md5" \
    --data-urlencode "deviceId=$LOOKDOOR_DEVICE_ID" \
    --data-urlencode "userNum@$work/phone" \
    'https://api.lookdoor.cn/func/hjapp/user/v1/login.json' 2>/dev/null \
    || fail '门禁登录网络失败；未发送开门请求。' '开门失败，无法连接门禁服务。'
code=$(jsonfilter -i "$work/response" -e '@.code' 2>/dev/null) || code=''
[ "$code" = 200 ] || fail '门禁登录未成功或响应无效；未发送开门请求，请检查扩展中的账号配置。' '开门失败，门禁登录未成功，请检查账号授权。'

log '登录成功，发送一次开门请求。'
curl -fsS --connect-timeout 4 --max-time 7 --max-filesize 32768 \
    -o "$work/response" -b "$work/cookie" \
    --data-urlencode "equipmentId=$LOOKDOOR_EQUIPMENT_ID" \
    --data-urlencode "userNum@$work/phone" \
    'https://api.lookdoor.cn/func/hjapp/house/v1/pushOpenDoorBySn.json' 2>/dev/null \
    || fail '开门接口网络异常，实际结果未知；没有自动重试，请查看门的状态。' '开门结果未确认，请查看玻璃门状态。'
code=$(jsonfilter -i "$work/response" -e '@.code' 2>/dev/null) || code=''
[ "$code" = 200 ] || fail '开门接口未返回成功或响应无效；没有自动重试，请查看门的状态。' '开门接口未返回成功，请查看玻璃门状态。'
log '开门接口返回成功（code=200），开门指令已发送；门体实际动作请现场确认。'
say_result '开门指令已成功发送。'
