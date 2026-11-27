# Embedded in xz_watch.sh by build_xz_pause_update.py.
if [ "${1:-}" = --mcp-task-session ] || [ "${1:-}" = --mcp-task-state ]; then
  task_root=/tmp/lx06-xz-mcp-task
  current=$(readlink "$task_root/active") || exit 1
  case "$current" in session.[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]) ;; *) exit 1;; esac
  [ -d "$task_root/$current" ] || exit 1
  if [ "$1" = --mcp-task-session ]; then
    [ "$#" -eq 1 ] || exit 2
    printf '%s\n' "$current"
    exit
  fi
  [ "$#" -eq 4 ] || [ "$#" -eq 5 ] || exit 2
  [ "$current" = "$2" ] || exit 1
  task_id=$3
  task_state=$4
  task_query=${5:-}
  case "$task_id" in ''|*[!a-zA-Z0-9_-]*) exit 2;; esac
  [ "${#task_id}" -le 48 ] || exit 2
  case "$task_state" in running|ready|queried) ;; *) exit 2;; esac
  if [ "$#" -eq 5 ]; then
    export LC_ALL=C
    [ -n "$task_query" ] && [ "${#task_query}" -le 160 ] || exit 2
    [ "$(printf '%s' "$task_query" | tr -d '[:cntrl:]')" = "$task_query" ] || exit 2
  fi
  session_dir=$task_root/$current
  state_file=$session_dir/$task_id.state
  umask 077
  (
    flock -x 9 || exit 1
    [ "$(readlink "$task_root/active")" = "$current" ] || exit 1
    now=$(date +%s) || exit 1
    active=0
    retained=0
    for item in "$session_dir"/*.state; do
      [ -f "$item" ] || continue
      written=$(date -r "$item" +%s) || exit 1
      if [ "$((now - written))" -gt 90 ]; then
        rm -f "$item" || exit 1
        continue
      fi
      read -r version old_id old_state < "$item" || exit 1
      retained=$((retained + 1))
      [ "$old_state" = queried ] || active=$((active + 1))
    done
    if [ -f "$state_file" ]; then
      {
        read -r version old_id old_state || exit 1
        [ "$old_id" = "$task_id" ] || exit 1
        case "$old_state:$task_state" in
          queried:queried) exit 0;;
          queried:*|ready:running) exit 1;;
        esac
        if [ "$version" = v2 ] && [ -z "$task_query" ]; then
          IFS= read -r task_query || exit 1
        fi
      } < "$state_file"
    else
      [ "$task_state" != queried ] || exit 0
      [ "$active" -lt 8 ] && [ "$retained" -lt 32 ] || exit 75
    fi
    draft=$(mktemp "$task_root/.task.XXXXXX") || exit 1
    trap 'rm -f "$draft"' EXIT
    if [ -n "$task_query" ]; then
      printf 'v2 %s %s\n%s\n' "$task_id" "$task_state" "$task_query" > "$draft" || exit 1
    else
      printf 'v1 %s %s\n' "$task_id" "$task_state" > "$draft" || exit 1
    fi
    mv -f "$draft" "$state_file"
  ) 9>> "$session_dir/lock"
  exit $?
fi
