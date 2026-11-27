# Use the device's official token when a manual token was never configured.
xz_ota_token() (
  mac=$(tr -d '\r\n' </sys/class/net/wlan0/address) || exit 1
  cid=$(tr -d '\r\n' <"$XZ_CLIENT_ID_FILE") || exit 1
  printf '%s\n' "$mac" | grep -Eq '^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$' || exit 1
  case "$cid" in ''|*[!A-Za-z0-9_-]*) exit 1;; esac
  [ "${#cid}" -le 128 ] || exit 1
  body=$(printf '{"version":2,"language":"zh-CN","mac_address":"%s","uuid":"%s","chip_model_name":"esp32s3","application":{"name":"xiaozhi","version":"1.0.0","elf_sha256":"1"},"board":{"type":"lx06","mac":"%s"}}' "$mac" "$cid" "$mac")
  response=$(curl -fsS --proto '=https' --connect-timeout 3 --max-time 6 --max-filesize 16384 \
    -H 'Content-Type: application/json' -H 'Activation-Version: 1' \
    -H "Device-Id: $mac" -H "Client-Id: $cid" -H 'User-Agent: lx06/1.0.0' \
    --data "$body" 'https://api.tenclass.net/xiaozhi/ota/' 2>/dev/null) || exit 1
  [ -z "$(printf '%s' "$response" | jsonfilter -e '@.activation' 2>/dev/null)" ] || exit 1
  url=$(printf '%s' "$response" | jsonfilter -e '@.websocket.url' 2>/dev/null) || exit 1
  [ "$url" = 'wss://api.tenclass.net/xiaozhi/v1/' ] || exit 1
  token=$(printf '%s' "$response" | jsonfilter -e '@.websocket.token' 2>/dev/null) || exit 1
  case "$token" in ''|*[!A-Za-z0-9._~+/=-]*) exit 1;; esac
  [ "${#token}" -le 512 ] || exit 1
  printf %s "$token"
)

if [ "${1:-}" = --launch-client ]; then
  [ "$#" -eq 1 ] && [ -n "${XZ_CLIENT_ID_FILE:-}" ] && [ -n "${XZ_MIC_FIFO:-}" ] || exit 2
  case "${XZ_ACCESS_TOKEN:-}" in
    ''|REPLACE_WITH_*)
      XZ_ACCESS_TOKEN=$(xz_ota_token) || { log '小智连接令牌获取失败，请检查绑定或网络'; exit 5; }
      export XZ_ACCESS_TOKEN
      ;;
  esac
  exec "$BIN"
fi
