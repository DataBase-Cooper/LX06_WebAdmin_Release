#!/bin/sh
# Read-only training-recording diagnostic for the stored wakewords.
# This legacy entry never supplies independent acceptance samples. Use the
# host wake_matrix.py ledger entry for explicit purpose isolation and A/B.
# Recordings and models stay on the device; this script does not upload them.
set -eu
base=/usr/lib/lx06/wakeword
data=/data/wakeword
tpl=$base/wakeword_template
conf=$data/wakeword.conf
[ -x "$tpl" ] || { echo '检测器不存在，无法交叉测试'; exit 2; }
[ -r "$conf" ] || { echo '没有唤醒词配置'; exit 2; }
set --
words=0
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in word=*) ;; *) continue ;; esac
    name= enabled=1 model= th=
    for kv in $(printf '%s' "$line" | tr '|' ' '); do
        case "$kv" in
            word=*) name=${kv#word=} ;;
            enabled=*) enabled=${kv#enabled=} ;;
            threshold=*) th=${kv#threshold=} ;;
        esac
    done
    [ "$enabled" = 1 ] && [ -n "$name" ] && [ -n "$th" ] || continue
    model=$data/models/$name.lwt
    [ -s "$model" ] || continue
    recs=
    nrec=0
    i=1
    while [ "$i" -le 10 ]; do
        if [ -f "$data/recs/$name/$i.wav" ]; then
            recs="$recs $data/recs/$name/$i.wav"
            nrec=$((nrec + 1))
        fi
        i=$((i + 1))
    done
    [ "$nrec" -ge 1 ] || continue
    if [ "$nrec" -lt 6 ]; then
        echo "录音不足：$name 只有 ${nrec} 条，建议至少 6 条" >&2
    fi
    set -- "$@" -- "$name=$model:$th" $recs
    words=$((words + 1))
done < "$conf"
[ "$words" -ge 1 ] || { echo '没有可测试的已启用模型'; exit 2; }
# The first "--" from the loop would be eaten as an option separator; drop it.
shift
echo "训练录音诊断交叉测试 purpose=train，词数=$words；不计独立验收"
# shellcheck disable=SC2086
"$tpl" matrix --policy norm --phases 4 "$@"
