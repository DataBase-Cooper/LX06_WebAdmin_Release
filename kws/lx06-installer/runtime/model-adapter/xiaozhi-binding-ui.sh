# Check the device binding without starting audio.
binding_cache_key() {
  [ -n "${BASE:-}" ] && [ -s "$BASE/client_id" ] &&
    [ -r /sys/class/net/wlan0/address ] || return 1
  cat "$BASE/client_id" /sys/class/net/wlan0/address | sha256sum | cut -d' ' -f1
}

binding_web_load() (
  key=$(binding_cache_key) || key=
  if [ -n "$key" ] && [ "$(cat /tmp/lx06-xz-bound 2>/dev/null)" = "$key" ]; then
    config=$("$SETTINGS" show "$0") || { printf '%s\n' "$config"; exit 1; }
    values=$(printf '%s' "$config" | /usr/bin/lx06-model-tool get values) || exit 1
    printf '{"step":"configure","values":%s}\n' "$values"
  else
    binding_web_event
  fi
)

binding_web_event() (
  config=$("$SETTINGS" show "$0") || { printf '%s\n' "$config"; exit 1; }
  config_values=$(printf '%s' "$config" | /usr/bin/lx06-model-tool get values) || exit 1
  binding_result=1
  key=$(binding_cache_key) || key=
  log() { :; }
  say() {
    case "$*" in
      '绑定码 '*)
        message=$*; code=${message#绑定码 }; code=${code%% *}
        case "$code" in
          [0-9][0-9][0-9][0-9][0-9][0-9])
            [ -z "$key" ] || rm -f /tmp/lx06-xz-bound
            printf '{"step":"pair","message":"请在小智控制台添加设备并填写配对码，绑定后自动继续。","linkLabel":"打开小智控制台","link":"https://xiaozhi.me/console/agents","code":"%s","values":%s}\n' "$code" "$config_values"; binding_result=0; return;;
        esac;;
      '小智已绑定，无需验证码')
        if [ -n "$key" ]; then
          temp=$(umask 077; mktemp /tmp/lx06-xz-bound.XXXXXX) &&
            { printf '%s\n' "$key" > "$temp" && mv -f "$temp" /tmp/lx06-xz-bound || rm -f "$temp"; }
        fi
        printf '{"step":"configure","message":"小智已绑定。","values":%s}\n' "$config_values"; binding_result=0; return;;
    esac
    printf '%s\n' '{"message":"暂未获取到绑定码，请稍后再试。"}'
  }
  publish_bind_code
  exit "$binding_result"
)
