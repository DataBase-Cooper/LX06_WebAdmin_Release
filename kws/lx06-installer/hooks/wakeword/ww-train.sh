#!/bin/sh
# Publish through ww_store (directory fd, exclusive stage file, fsync).
# Do not copy onto a fixed .pending/.tmp name inside the wakeword directory.
set -eu
result() {
    if [ "${LX06_WW_WEB_RETRAIN:-}" = 1 ]; then
        printf 'LX06_WW_RESULT %s\n' "$*"
    else
        printf '%s\n' "$*"
    fi
}
fail() {
    status=$1
    shift
    result "$*"
    exit "$status"
}
name=${1:-}
base=/usr/lib/lx06/wakeword
data=/data/wakeword
joint_report=/tmp/lx06-wake-joint
lock=/tmp/lx06-ww-mutate.lock
if [ "${2:-}" = --epoch-trial ]; then
    case "${3:-}" in /tmp/lx06-kwe2-trial.??????) ;; *) exit 2 ;; esac
    case "${3##*.}" in *[!A-Za-z0-9]*) exit 2 ;; esac
    [ "$(cd -P "$3" && pwd -P)" = "$3" ] || exit 2
    [ "$(cat "$3/capture-domain")" = raw-slot4-hp65-g8-kwe2-v2 ] || exit 2
    base=$(cd -P "$(dirname "$0")" && pwd -P)
    data=$3
    export LX06_WAKE_FORENSICS_DIR="$data/diagnostics"
    joint_report=$data/joint
    lock=$data/trial.lock
    shift 2
fi
work=/tmp/ww-train-$$
case "$name" in ''|*'/'*|*'\'*|*'"'*|*'..'*|*'|'*|*'='*|*'
'*) exit 2 ;; esac
if [ "${LX06_WW_LOCK_HELD:-}" != 1 ]; then exec 9>"$lock"; fi
flock -n 9 || fail 1 '正在录音或训练'
(umask 077; mkdir "$work") || fail 1 '训练临时空间不可用'
cleanup() { rm -rf "$work/shadow"; rm -f "$work/model" "$work/model.tmp."* "$work/calibration" "$work/detail" "$work/conf"; rmdir "$work" 2>/dev/null || true; }
trap cleanup EXIT
trap 'exit 1' INT TERM
check_sample=0
if [ "${2:-}" = --check-append ] || [ "${2:-}" = --check-replace ]; then
    # Validate the new sample against the full matrix before changing DATA.
    # Other words' recordings are read in place; every writable path is private.
    slot=${3:-}
    case "$2:$slot" in --check-append:[1-5]|--check-replace:[1-9]|--check-replace:10) ;; *) exit 2 ;; esac
    check_sample=1
    staged=$work/shadow
    mkdir -p "$staged/models" "$staged/recs/$name"
    cp "$data/wakeword.conf" "$staged/wakeword.conf"
    for item in "$data"/models/*.lwt; do
        [ ! -f "$item" ] || cp "$item" "$staged/models/"
    done
    for worddir in "$data"/recs/*; do
        [ ! -d "$worddir" ] || [ "$worddir" = "$data/recs/$name" ] || ln -s "$worddir" "$staged/recs/"
    done
    for item in "$data/recs/$name"/*.wav; do
        [ ! -f "$item" ] || cp "$item" "$staged/recs/$name/"
    done
    if [ "$2" = --check-replace ]; then
        [ -f "$staged/recs/$name/$slot.wav" ] || exit 2
    else
        [ ! -e "$staged/recs/$name/$slot.wav" ] || exit 2
    fi
    cp "$4" "$staged/recs/$name/$slot.wav"
    data=$staged
    joint_report=$staged/joint
    "$base/ww_store" mark-unready "$data" "$name"
fi
conf=$data/wakeword.conf
set --
for i in 1 2 3 4 5 6 7 8 9 10; do
    [ ! -f "$data/recs/$name/$i.wav" ] || set -- "$@" "$data/recs/$name/$i.wav"
done
[ "$#" -ge 3 ] || fail 2 '请先录满 3 条'
for tool in wakeword_template wakeword_calibrate ww_store; do
    [ -x "$base/$tool" ] || fail 2 '固件训练组件缺失，请更新固件'
done
identity=$("$base/wakeword_template" info) || fail 2 '固件训练组件无法运行'
feature=$(printf '%s\n' "$identity" | awk '$1=="FEATURE" {sub("model_version=", "", $3); id=$2 ":" $3; if($5~/^calibration=/){sub("calibration=", "", $5); id=id ":" $5} print id}')
[ "$feature" = mfcc-lifter32:8:duration-bank2-candidate2-norm ] || fail 2 '训练组件版本不匹配'
for recording in "$@"; do
    if ! "$base/wakeword_template" quality "$recording" >&2; then
        n=${recording##*/}; n=${n%.wav}
        fail 2 "第${n}条录音质量不合格，请重录"
    fi
done
"$base/wakeword_template" train "$name" "$work/model" "$@" >&2 || fail 1 '模型生成失败，请查看唤醒日志'
# First calculate the new model's own gate. Cross tightening runs only after
# the raw all-word matrix has been saved and conflicts default-disabled.
rc=0
nice -n 10 "$base/wakeword_calibrate" "$work/model" "$@" >"$work/calibration" 2>"$work/detail" || rc=$?
if [ "$rc" -ne 0 ]; then
    cat "$work/detail" >&2
    bad=$(awk '$1=="CALIBRATION_REJECT" {sub("rec=", "", $2); print $2}' "$work/detail")
    reason=$(awk '$1=="CALIBRATION_REJECT" {sub("reason=", "", $3); print $3}' "$work/detail")
    if [ -n "$bad" ]; then
        position=0
        for recording in "$@"; do
            position=$((position+1))
            if [ "$position" = "$bad" ]; then bad=${recording##*/}; bad=${bad%.wav}; break; fi
        done
        if [ "$reason" = quiet ]; then
            fail 1 "第${bad}条录音声音太弱，请重录"
        fi
        fail 1 "第${bad}条录音差异过大，请重录"
    fi
    if grep -q CALIBRATION_CONFLICT "$work/detail"; then
        fail 1 '录音与其他唤醒词过于相似，请重录'
    fi
    if [ "$rc" -eq 126 ] || [ "$rc" -eq 127 ]; then
        fail 2 '固件校准组件无法运行'
    fi
    fail 1 '自动校准失败，请查看唤醒日志'
fi
cat "$work/detail" >&2
threshold=$(awk -v feature="$feature" '$1=="CALIBRATED" && $2 ~ /^[0-9]+[.][0-9]+$/ && $2>0 && $2<=0.26 && $3==feature {print $2}' "$work/calibration")
[ -n "$threshold" ] || fail 1 '录入不合格，请重新录入'
# The store clamps to the last accepted compatible gate atomically with
# candidate publication. A rejected first attempt never becomes that limit.
digest=$(sha256sum "$work/model") || fail 1 '无法读取生成的模型'
digest=${digest%% *}
"$base/ww_store" publish-candidate "$data" "$name" "$work/model" "$threshold" "$feature" "$digest" ||
    fail 1 '模型保存失败，请检查 DATA 剩余空间'
# The candidate remains disabled if another word's inputs or calibration fail.
if ! joint=$(LX06_WW_LOCK_HELD=1 nice -n 10 "$base/wakeword_calibrate" joint "$data" "$joint_report"); then
    fail 1 '模型已保存但校准未完成，请查看唤醒日志'
fi
printf '%s\n' "$joint" >&2
if awk -F'|' -v word="word=$name" '$1==word {for(i=2;i<=NF;i++)if($i=="p05_conflict=1")bad=1}END{exit !bad}' "$conf"; then
    retry=$(printf '%s\n' "$joint" | awk -v w="$name" '$1=="JOINT_REJECT" && $2==w && $3=="self" && $4~/^([1-9]|10)$/ {list=list sep $4;sep="、"} END{print list}')
    if [ "$check_sample" -eq 1 ] && [ -n "$retry" ]; then
        fail 1 '新录音与现有录音组合未通过校准，请重录这一条'
    fi
    [ -z "$retry" ] || fail 1 "第${retry}条录音未通过校准，请重录"
    if printf '%s\n' "$joint" | grep -Fxq "JOINT_REJECT $name cross"; then
        fail 1 '录音与其他声音过于相似，请重录或换词'
    fi
    fail 1 '校准未通过，请重录'
fi
result '训练完成'
