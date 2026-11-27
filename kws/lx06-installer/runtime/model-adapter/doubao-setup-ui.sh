# UI-only probe; never opens the microphone/player or steals a voice session.
doubao_setup_event() {
  _config=$("$SETTINGS" show "$0") || { printf '%s\n' "$_config"; return 1; }
  _values=$(printf '%s' "$_config" | /usr/bin/lx06-model-tool get values) || return 1
  printf '{"step":"%s","message":"%s","values":%s}\n' "$1" "$2" "$_values"
}
doubao_setup_save() {
  _step=$(doubao_setup_step)
  _request=$(cat) || return 2
  _fields=$(printf '%s' "$_request" | /usr/bin/lx06-model-tool get values) || return 2
  LX06_DB_SETUP_KEY=$(doubao_setup_field DB_API_KEY) || return 2
  LX06_DB_SETUP_ENDPOINT=$(doubao_setup_field DB_ENDPOINT) || return 2
  LX06_DB_SETUP_VOICE=$(doubao_setup_field DB_VOICE) || return 2
  LX06_DB_SETUP_INSTRUCTIONS=$(doubao_setup_field DB_INSTRUCTIONS) || return 2
  LX06_DB_SETUP_FORMAT=$(doubao_setup_field DB_AUDIO_FORMAT) || return 2
  if [ -z "$LX06_DB_SETUP_KEY" ] && [ "$_step" = connect ]; then
    doubao_setup_event connect '请输入豆包 API Key。'; return 1
  fi
  export LX06_DB_SETUP_KEY LX06_DB_SETUP_ENDPOINT LX06_DB_SETUP_VOICE
  export LX06_DB_SETUP_INSTRUCTIONS LX06_DB_SETUP_FORMAT
  if [ "$1" = save ] && [ "$_step" = configure ] && [ -z "$LX06_DB_SETUP_KEY" ] &&
     "$SETTINGS" run "$0" /bin/sh -c '
       [ "$DB_ENDPOINT" = "$LX06_DB_SETUP_ENDPOINT" ] &&
       [ "$DB_VOICE" = "$LX06_DB_SETUP_VOICE" ] &&
       [ "$DB_INSTRUCTIONS" = "$LX06_DB_SETUP_INSTRUCTIONS" ] &&
       [ "$DB_AUDIO_FORMAT" = "$LX06_DB_SETUP_FORMAT" ]
     '; then
    doubao_setup_commit '豆包设置已保存。'
    return $?
  fi
  _transport=${DC_TRANSPORT_ADAPTER:-/usr/lib/lx06/lx06-transport-adapter}
  [ -x "$_transport" ] && [ "$("$_transport" --protocol 2>/dev/null)" = LX06DCTRANSPORT/1 ] || {
    doubao_setup_event "$_step" '固件缺少连接验证组件，请更新固件。'; return 1;
  }
  # Start with the saved key when the field is blank. Probe the submitted
  # connection settings before committing that same request to DATA.
  if ! _probe=$("$SETTINGS" run "$0" /bin/sh -c '
    [ -z "$LX06_DB_SETUP_KEY" ] || export DB_API_KEY="$LX06_DB_SETUP_KEY"
    export DB_ENDPOINT="$LX06_DB_SETUP_ENDPOINT" DB_VOICE="$LX06_DB_SETUP_VOICE"
    export DB_INSTRUCTIONS="$LX06_DB_SETUP_INSTRUCTIONS" DB_AUDIO_FORMAT="$LX06_DB_SETUP_FORMAT"
    unset LX06_DB_SETUP_KEY LX06_DB_SETUP_ENDPOINT LX06_DB_SETUP_VOICE
    unset LX06_DB_SETUP_INSTRUCTIONS LX06_DB_SETUP_FORMAT
    exec "$1" --check --provider doubao
  ' probe "$_transport" 2>&1); then
    case "$_probe" in
      *'websocket upgrade HTTP=401'*) doubao_setup_event "$_step" '服务拒绝了 API Key（HTTP 401），请核对密钥。';;
      *'websocket upgrade HTTP=403'*) doubao_setup_event "$_step" '服务拒绝连接（HTTP 403），请核对密钥和端到端实时语音权限。';;
      *'websocket upgrade HTTP=429'*) doubao_setup_event "$_step" '服务限制连接（HTTP 429），请检查配额或稍后重试。';;
      *) doubao_setup_event "$_step" '未建立已认证的豆包会话，请检查网络和服务配置。';;
    esac
    return 1
  fi
  unset LX06_DB_SETUP_KEY LX06_DB_SETUP_ENDPOINT LX06_DB_SETUP_VOICE
  unset LX06_DB_SETUP_INSTRUCTIONS LX06_DB_SETUP_FORMAT
  doubao_setup_commit '豆包连接验证通过，配置已保存。'
}
doubao_setup_commit() {
  # Both Web actions use the settings helper's canonical save request.
  if ! _saved=$(printf '{"action":"save","values":%s}\n' "$_fields" | "$SETTINGS" save "$0"); then
    printf '%s\n' "$_saved"
    return 2
  fi
  doubao_setup_event configure "$1"
}
doubao_setup_field() {
  printf '%s' "$_fields" | /usr/bin/lx06-model-tool get "$1"
}
doubao_setup_has_key() {
  "$SETTINGS" run "$0" /bin/sh -c 'test -n "$DB_API_KEY"' >/dev/null 2>&1
}
doubao_setup_step() {
  if doubao_setup_has_key; then
    printf '%s' configure
  else
    printf '%s' connect
  fi
}
doubao_setup_load() { doubao_setup_event "$(doubao_setup_step)" ''; }
