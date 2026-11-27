#!/bin/sh
# Device qualification only. Audio and calibration stay in this private TMP profile.
# No command in this entry point submits a detected word to a wake consumer.
set -eu
umask 077
base=$(cd -P "$(dirname "$0")" && pwd -P)
domain=raw-slot4-hp65-g8-kwe2-v2
op=${1:-}
if [ "$op" = init ] && [ "$#" = 1 ]; then
    root=$(mktemp -d /tmp/lx06-kwe2-trial.XXXXXX)
    printf '%s\n' "$domain" > "$root/capture-domain"
    : > "$root/wakeword.conf"
    mkdir "$root/recs" "$root/models"
    printf '%s\n' "$root"
    exit
fi
[ "$#" = 3 ] || exit 2
root=$2
name=$3
case "$root" in /tmp/lx06-kwe2-trial.??????) ;; *) exit 2 ;; esac
case "${root##*.}" in *[!A-Za-z0-9]*) exit 2 ;; esac
[ "$(cd -P "$root" && pwd -P)" = "$root" ] && [ "$(cat "$root/capture-domain")" = "$domain" ] || exit 2
cjk3=$(printf '\344[\270-\277][\200-\277]|[\345-\351][\200-\277][\200-\277]')
printf '%s\n' "$name" | LC_ALL=C grep -Eq "^($cjk3){3,4}$" || exit 2
exec 9>"$root/trial.lock"
flock -n 9 || exit 1
case "$op" in
    train) LX06_WW_LOCK_HELD=1 exec sh "$base/ww-train.sh" "$name" --epoch-trial "$root" ;;
    record|record-cued|listen) ;;
    *) exit 2 ;;
esac
work=$(mktemp -d "$root/job.XXXXXX")
pid=
cap_pid=
cleanup() {
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; fi
    if [ -n "$cap_pid" ]; then kill "$cap_pid" 2>/dev/null || :; wait "$cap_pid" 2>/dev/null || :; fi
    rm -f "$work/stream" "$work/utterance.wav" "$work/ready" "$work/gate"
    rmdir "$work"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
reader=$base/../xiaoai/processed-mic-read
tpl=$base/wakeword_template
mkfifo "$work/stream"
export LX06_WAKE_CLOUD_VERIFY=off LX06_WAKE_FORENSICS=0 LX06_WAKE_DATA="$root"
if [ "$op" = record ] || [ "$op" = record-cued ]; then
    recdir=$root/recs/$name
    mkdir -p "$recdir"
    i=1
    while [ "$i" -le 5 ] && [ -f "$recdir/$i.wav" ]; do i=$((i+1)); done
    [ "$i" -le 5 ] || { echo '已录满 5 条' >&2; exit 2; }
    set -- --name enrollment --kws-epoch --dur 14
    if [ "$op" = record-cued ]; then
        set -- "$@" --ready-file "$work/ready" --gate-file "$work/gate" --preroll-ms 400
    fi
    "$tpl" capture-epoch "$work/utterance.wav" 8 < "$work/stream" 9>&- & cap_pid=$!
    "$reader" "$@" > "$work/stream" 9>&- & pid=$!
    if [ "$op" = record-cued ]; then
        retry=0
        while [ ! -f "$work/ready" ]; do
            kill -0 "$pid" 2>/dev/null && [ "$retry" -lt 40 ] || { echo '采集尚未就绪' >&2; exit 1; }
            sleep 0.1
            retry=$((retry+1))
        done
        printf 'READY\n'
        /usr/bin/aplay -q -D notify /usr/lib/lx06/wakeword/ww-ready.wav 9>&-
        touch "$work/gate"
        printf 'WINDOW\n'
    fi
    wait "$cap_pid"
    cap_pid=
    "$tpl" quality "$work/utterance.wav" >&2
    if ! awk -F'|' -v w="word=$name" '$1==w{found=1} END{exit !found}' "$root/wakeword.conf"; then
        printf 'word=%s|enabled=1\n' "$name" >> "$root/wakeword.conf"
    else
        "$base/ww_store" mark-unready "$root" "$name"
    fi
    mv "$work/utterance.wav" "$recdir/$i.wav"
    printf '已保存第 %s 条独立录音\n' "$i"
else
    gate=$(awk -F'|' -v w="word=$name" '$1==w{for(i=2;i<=NF;i++){split($i,p,"=");f[p[1]]=p[2]}
        if(f["enabled"]=="1" && f["p05_conflict"]!="1" && f["p05_pending"]!="1" &&
           f["feature"]=="mfcc-lifter32:8:duration-bank2-candidate2-norm" &&
           f["threshold"] ~ /^[0-9]+[.][0-9]+$/ && f["threshold"]+0>0 && f["threshold"]+0<=0.26)
            print f["threshold"], f["model_sha256"]}' "$root/wakeword.conf")
    set -- $gate
    [ "$#" = 2 ] || { echo '请先完成训练与校准' >&2; exit 2; }
    model=$root/models/$name.lwt
    sha=$(sha256sum "$model")
    [ "${sha%% *}" = "$2" ] || exit 2
    "$reader" --name enrollment --kws-epoch --dur 15 > "$work/stream" 9>&- & pid=$!
    "$tpl" detect-epoch "$name=$model:$1" < "$work/stream"
    wait "$pid"
    pid=
fi
