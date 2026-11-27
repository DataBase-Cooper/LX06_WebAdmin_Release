# Show deployed scripts declaring the local home MCP interface.
home_mcp_options() {
  printf '%s' '{"options":{"XZ_HOME_MCP_SCRIPT":[{"value":"none","label":"请选择已上传的脚本"}'
  _home_count=0
  for _home_path in /data/lx06/apps/*.sh; do
    [ -f "$_home_path" ] && [ ! -L "$_home_path" ] || continue
    _home_name=${_home_path##*/}
    case "$_home_name" in *[!A-Za-z0-9._-]*) continue;; esac
    [ "${#_home_name}" -le 80 ] || continue
    head -c 1024 "$_home_path" | grep -Fxq '# LX06-HOME-MCP: 1' || continue
    printf ',{"value":"%s","label":"%s"}' "$_home_name" "$_home_name"
    _home_count=$((_home_count + 1))
    [ "$_home_count" -lt 63 ] || break
  done
  printf '%s\n' ']}}'
}
