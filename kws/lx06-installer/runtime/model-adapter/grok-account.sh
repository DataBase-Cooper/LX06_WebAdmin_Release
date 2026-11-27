# Provider flow is replaceable uploadable source, never compiled into firmware.
PROVIDER=grok
TOKEN_URL=https://auth.x.ai/oauth2/token
BASE_URL=https://cli-chat-proxy.grok.com/v1
provider_preconnect() { HTTP_WARM_URL="$BASE_URL/models"; }
provider_login_ready() {
    if (models); then
        ui_settings configure 'Grok 登录成功，已读取账户可用模型。'
    else
        ui_settings configure 'Grok 登录成功，暂未取得模型目录。请点击更新模型列表。'
    fi
}
REQUIRED_SCOPE=grok-cli:access
MODEL_LIST_KEY=data
MODEL_ID_KEY=id
MODEL_LABEL_KEY=name
# Merge current and alternate account envelopes while preserving first-seen order.
MODEL_LIST_SPECS='data|id|name|-|- data|id|display_name|-|- models|id|name|-|- models|slug|display_name|-|-'
GROK_CLIENT=b1a00492-073a-47ea-816f-4c329264a828
# Minimum protocol version returned by the real Grok service on 2026-10-30.
# Keep the adapter's own User-Agent; never pretend this ARM client is x86 CLI.
GROK_PROTOCOL_VERSION=1.0.13
provider_headers() {
    printf 'header = "x-grok-client-version: %s"\n' "$GROK_PROTOCOL_VERSION"
    printf '%s\n' 'header = "X-XAI-Token-Auth: xai-grok-cli"' 'header = "x-grok-client-identifier: grok-pager"' 'user-agent = "LX06-XiaoAI-WebAdmin/experimental (Linux; armv7l)"'
}
provider_request_options() {
    # Official Responses field; these exact IDs default to high reasoning.
    # Low reduces pre-text reasoning work without changing the selected model.
    # Do not send none or Chat Completions' reasoning_effort to Responses.
    # https://docs.x.ai/developers/model-capabilities/text/reasoning
    # Other account/custom IDs need separate capability verification.
    case "$1" in grok-4.7|grok-4.6|grok-4.5) printf ',"reasoning":{"effort":"low"}';; esac
}
refresh_form() { form grant_type "$WORK/grant" client_id "$WORK/client" refresh_token "$WORK/refresh"; }
login() {
    put client "$GROK_CLIENT"
    put scope 'openid profile email offline_access grok-cli:access api:access'
    form client_id "$WORK/client" scope "$WORK/scope"
    http POST https://auth.x.ai/oauth2/device/code "$WORK/device" || fail 'Grok 未接受设备登录请求。'
    get device_code "$WORK/device" > "$WORK/devicecode" || fail 'Grok 未提供设备码。'
    link=$(get verification_uri "$WORK/device" 2>/dev/null || :)
    case "$link" in https://auth.x.ai/*|https://accounts.x.ai/*|https://grok.com/*) :;; *) fail 'Grok 返回未获允许的登录网址。';; esac
    code=$(get user_code "$WORK/device" 2>/dev/null || :)
    [ -n "$code" ] || fail 'Grok 未提供验证码。'
    interval=$(get interval "$WORK/device" 2>/dev/null || printf 5)
    case "$interval" in ''|*[!0-9]*|????*) fail '设备登录轮询间隔无效。';; esac
    [ "$interval" -ge 5 ] || interval=5
    [ "$interval" -le 30 ] || fail '设备登录轮询间隔过长。'
    seconds=$(get expires_in "$WORK/device" 2>/dev/null || :)
    expiry_valid "$seconds" || fail '设备登录有效期无效。'
    [ "$seconds" -le 210 ] || seconds=210
    deadline=$(( $(date +%s) + seconds ))
    put message '请打开官方授权页并输入验证码。完成授权后回到这里；测试使用本人账号额度。'
    put link "$link"; put code "$code"
    put ui_step authorize
    "$TOOL" object message "$WORK/message" link "$WORK/link" code "$WORK/code" step "$WORK/ui_step"
    put grant urn:ietf:params:oauth:grant-type:device_code
    form grant_type "$WORK/grant" client_id "$WORK/client" device_code "$WORK/devicecode"
    while [ "$(date +%s)" -lt "$deadline" ]; do
        sleep "$interval"
        if http POST "$TOKEN_URL" "$WORK/token"; then
            # Device flow credentials come from the TLS-authenticated token
            # endpoint. Do not decode ES256 ID tokens as verified identity.
            : > "$WORK/subject"; SAVING_REFRESH=0; save_tokens
            return
        fi
        error=$(get error "$WORK/token" 2>/dev/null || :)
        case "$error" in authorization_pending) :;; slow_down) interval=$((interval + 5));; access_denied) fail '你已拒绝授权，原账户未改变。';; expired_token) fail '验证码已过期，请重新登录。';; *) fail 'Grok 登录失败，原账户未改变。';; esac
    done
    fail '本次登录等待已结束，请重新点击登录。'
}
