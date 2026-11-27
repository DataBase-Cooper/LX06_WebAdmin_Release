#!/bin/sh
# LX06 enrollment: independent copy of DSP output, before the official VAD.
#AUDIO-PATH: native-vpm
# Capture is READY before the tone. Short real-frame preroll retains responses
# during the cue's quiet tail and ALSA drain; it never inserts synthetic samples.
# No VOIP events, shared FIFO reader, silence insertion or engine restart.
name="$1"
test="${2:-}"
max_new_samples=5
replace="${3:-}"
collection=
case "$test" in
    acceptance|calibration|diagnostic)
        collection=$test
        case "$replace" in normal|far|soft) ;; *) exit 2 ;; esac ;;
    ''|test) ;;
    *) exit 2 ;;
esac
base=/usr/lib/lx06/wakeword
tpl=$base/wakeword_template
reader=/usr/lib/lx06/xiaoai/processed-mic-read
data=/data/wakeword
validation=/tmp/lx06-wake-validation-samples
recdir="$data/recs/$name"
model="$data/models/$name.lwt"
state=/tmp/lx06-ww-record-state
log_sink=/usr/bin/lx06-keeper
work=/tmp/ww-record-$$
lock=/tmp/ww-record-lock
cap_pid=
mic_pid=
tone_pid=
log_pid=
[ -n "$name" ] || exit 1
# webd validates the word; reject characters unsafe in paths/JSON for CLI too.
case "$name" in *'/'*|*'\'*|*'"'*|*'..'*|*'
'*) exit 2 ;; esac
mkdir -p "$recdir" "$data/models"
statef() {
    local t=0
    [ -n "$test" ] && t=1
    printf '{"status":"%s","word":"%s","test":%s,"detail":"%s"}' \
        "$1" "$name" "$t" "$(printf '%s' "${2:-}" | tr '\t\n\r' '   ' | sed 's/["\\]/ /g')" > "$state.$$" && mv -f "$state.$$" "$state"
}
fail() { statef error "$1"; exit 1; }
# The kernel releases this lock even after SIGKILL. Web workers inherit the
# same open file description on fd 9; CLI invocations acquire it themselves.
if [ "${LX06_WW_LOCK_HELD:-}" != 1 ]; then exec 9>/tmp/lx06-ww-mutate.lock; fi
flock -n 9 || exit 1
# A stale marker is safe to recover only after obtaining the real lock.
mkdir -p "$lock" || exit 1
mkdir "$work" || { rmdir "$lock"; exit 1; }
printf '%s\n' "$$" > "$lock/pid"
cleanup() {
    [ -n "$tone_pid" ] && kill "$tone_pid" 2>/dev/null
    [ -n "$mic_pid" ] && kill "$mic_pid" 2>/dev/null
    [ -n "$cap_pid" ] && kill "$cap_pid" 2>/dev/null
    [ -n "$mic_pid" ] && wait "$mic_pid" 2>/dev/null
    [ -n "$cap_pid" ] && wait "$cap_pid" 2>/dev/null
    exec 6>&-
    [ -z "$log_pid" ] || wait "$log_pid" 2>/dev/null || :
    rm -f "$work/audio" "$work/ready" "$work/gate" "$work/capture.wav" "$work/diagnostics" /tmp/ww-window-open /tmp/ww-cue-start
    rmdir "$work" 2>/dev/null
    rm -f "$lock/pid"
    rmdir "$lock" 2>/dev/null
}
trap cleanup EXIT
trap 'statef error 录音已取消; exit 1' INT TERM
exec 6>/dev/null
if [ -x "$log_sink" ] && mkfifo -m 600 "$work/diagnostics"; then
    exec 6<>"$work/diagnostics"
    if "$log_sink" log prepare-fifo >&6 9>&-; then
        "$log_sink" log wake <"$work/diagnostics" 6>&- 9>&- &
        log_pid=$!
    else
        exec 6>/dev/null
    fi
fi
[ -x "$reader" ] || fail "连续录音组件缺失"
[ -x "$tpl" ] || fail "唤醒模型组件缺失"
statef recording ""
rm -f /tmp/ww-window-open
prompt=$base/ww-prompt.mp3
[ -n "$test" ] && prompt=$base/ww-test.mp3
[ -s "$prompt" ] || fail "录音提示语缺失"
/usr/bin/miplayer -f "$prompt" >/dev/null 2>&1 6>&- &
tone_pid=$!
wait "$tone_pid" || fail "录音提示语播放失败"
tone_pid=
# Open both ends of a private pipe before readiness. Reader drains real frames
# while gated, so prompt/tone audio is never queued into the enrollment sample.
mkfifo "$work/audio" || fail "录音管道创建失败"
"$tpl" capture "$work/capture.wav" 8 < "$work/audio" 2>&6 6>&- &
cap_pid=$!
set -- --name enrollment --ready-file "$work/ready" --gate-file "$work/gate" --preroll-ms 400 --dur 14
GLIBC_TUNABLES=glibc.pthread.rseq=0 "$reader" "$@" \
    > "$work/audio" 2>&6 6>&- &
mic_pid=$!
i=0
while [ ! -f "$work/ready" ]; do
    kill -0 "$mic_pid" 2>/dev/null || fail "麦克风连续流未就绪"
    [ "$i" -lt 40 ] || fail "等待麦克风连续流超时"
    sleep 0.1
    i=$((i+1))
done
printf '%s record: ready pid=%s\n' "$(cut -d ' ' -f 1 /proc/uptime)" "$$" >&6 || :
beep=$base/ww-ready.wav
[ -s "$beep" ] || fail "录音提示音缺失"
# Same decoded multirounds_tone waveform, through the stock notification route.
# aplay drains actual audio; miplayer's delayed teardown is not a speech gate.
/usr/bin/aplay -q -D notify "$beep" >/dev/null 2>&1 6>&- &
tone_pid=$!
touch /tmp/ww-cue-start
wait "$tone_pid" || fail "录音提示音播放失败"
tone_pid=
touch "$work/gate" /tmp/ww-window-open
printf '%s record: window pid=%s\n' "$(cut -d ' ' -f 1 /proc/uptime)" "$$" >&6 || :
wait "$cap_pid"
rc=$?
cap_pid=
kill "$mic_pid" 2>/dev/null
wait "$mic_pid" 2>/dev/null
mic_pid=
[ "$rc" -eq 0 ] && [ -s "$work/capture.wav" ] || fail "未检测到有效语音，请在提示音后说出唤醒词"
if [ -n "$collection" ]; then
    quality=$("$tpl" quality "$work/capture.wav" 2>&1) || fail "$quality"
    detail=$(sh "$base/ww-sample-store.sh" "$validation" "$name" "$collection" "$replace" "$work/capture.wav") || fail "$detail"
    statef done "$detail 独立样本已保存；未评分、未训练、未修改门限"
elif [ -n "$test" ]; then
    sc=$("$tpl" score "$work/capture.wav" "$name=$model" 2>&6 6>&-) || fail "评分失败，请重新录制或检查模型"
    sc=$(printf '%s\n' "$sc" | awk '$1 == "SCORE" && $3 ~ /^-?[0-9]+([.][0-9]+)?$/ {print $3; exit}')
    [ -n "$sc" ] || fail "评分结果无效，请重新录制"
    statef done "score=$sc"
else
    if [ -n "$replace" ]; then
        case "$replace" in
            *[!0-9]*) fail "录音序号无效" ;;
        esac
        n=$((replace + 0))
        [ "$n" -ge 1 ] && [ "$n" -le 10 ] || fail "录音序号无效"
        [ -f "$recdir/$n.wav" ] || fail "要重录的录音不存在"
    else
        count=0
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ ! -f "$recdir/$i.wav" ] || count=$((count+1))
        done
        [ "$count" -lt "$max_new_samples" ] || fail "最多保留 5 条录音"
        n=1
        while [ "$n" -le "$max_new_samples" ] && [ -f "$recdir/$n.wav" ]; do n=$((n+1)); done
        [ "$n" -le "$max_new_samples" ] || fail "最多保留 5 条录音"
    fi
    quality=$("$tpl" quality "$work/capture.wav" 2>&1) || fail "$quality"
    set --
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -f "$recdir/$i.wav" ] && set -- "$@" "$recdir/$i.wav"
    done
    if [ "$#" -ge 3 ] && [ -s "$model" ]; then
        check=--check-append
        [ -z "$replace" ] || check=--check-replace
        statef training "正在检查新录音"
        detail=$(LX06_WW_LOCK_HELD=1 sh "$base/ww-train.sh" "$name" "$check" "$n" "$work/capture.wav" 2>&6 6>&-) ||
            fail "${detail:-新录音未通过校准}，原录音和模型已保留"
    fi
    if [ "$#" -ge 2 ]; then
        "$base/ww_store" mark-unready "$data" "$name" || fail "无法保存录入状态"
    fi
    mv "$work/capture.wav" "$recdir/$n.wav" || fail "录音保存失败"
    # Never let a failed replacement reuse the preceding recording's pinyin.
    rm -f "$recdir/$n.asr.json"
    # Always transcribe. The cloud-verify switch is not consulted.
    if [ -f "$base/ww-enroll-asr.sh" ]; then
        sh "$base/ww-enroll-asr.sh" "$recdir/$n.wav" "$recdir/$n.asr.json" || true
    fi
    asr=needs_rerecord
    if [ -f "$recdir/$n.asr.json" ] && grep -q '"status":"ok"' "$recdir/$n.asr.json"; then
        asr=ok
    fi
    set --
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -f "$recdir/$i.wav" ] && set -- "$@" "$recdir/$i.wav"
    done
    cnt=$#
    trained=0
    if [ "$cnt" -ge 3 ]; then
        statef training "录音已保存，正在训练并自动校准"
        detail=$(LX06_WW_LOCK_HELD=1 sh "$base/ww-train.sh" "$name" 2>&6 6>&-) || fail "$detail"
        trained=1
    fi
    if [ "$trained" = 1 ]; then statef done "$detail asr=$asr";
    else statef done "已录 $cnt 条，录满 3 条开始训练 asr=$asr"; fi
fi
exit 0
