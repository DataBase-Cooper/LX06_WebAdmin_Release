#!/bin/sh
# V14.3 开机：pstore(ramoops) 崩溃快照持久化 + last-crash 摘要
# pstore 在 RAM 里，重启即清；若上次启动是 crash 重启，本次开机就能拿到现场。
# 网页 /api/crash 和 SSH 侧都从这里读。
P=/sys/fs/pstore
D=/data/pstore-dump
[ -d "$P" ] || exit 0

# 有没有非空快照
found=0
for f in "$P"/*; do
  [ -e "$f" ] || continue
  sz=$(wc -c < "$f" 2>/dev/null)
  [ "${sz:-0}" -gt 0 ] && found=1
done
[ "$found" -eq 1 ] || exit 0

seq=$(date +%Y%m%d_%H%M%S 2>/dev/null)
[ -z "$seq" ] && seq=$(awk '{print int($1)}' /proc/uptime)
mkdir -p "$D/$seq"
# 每个快照文件上限 64KiB，保留最新尾部。
for f in "$P"/*; do
  [ -e "$f" ] || continue
  n=$(basename "$f")
  sz=$(wc -c < "$f" 2>/dev/null)
  [ "${sz:-0}" -gt 0 ] || continue
  if [ "${sz:-0}" -gt 65536 ]; then
    tail -c 65536 "$f" > "$D/$seq/$n" 2>/dev/null
  else
    cp "$f" "$D/$seq/$n" 2>/dev/null
  fi
done

# last-crash 摘要（最新 dump 的 pstore 尾部，便于 web/用户快速看）
{
  echo "== 崩溃快照 $seq（本次开机生成，上次启动疑似 crash）=="
  for f in "$P"/*; do
    [ -e "$f" ] || continue
    n=$(basename "$f")
    sz=$(wc -c < "$f" 2>/dev/null)
    [ "${sz:-0}" -gt 0 ] || continue
    echo
    echo "---- pstore/$n (${sz}B) 尾部 ----"
    tail -c 4096 "$f" 2>/dev/null
  done
} | tail -c 65536 > "$D/last-crash.txt" 2>/dev/null

# 只保留最近 2 份快照（/data 空间紧张）
ls -1dt "$D"/[0-9]* 2>/dev/null | tail -n +3 | while read -r old; do
  rm -rf "$old" 2>/dev/null
done
exit 0
