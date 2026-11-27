#!/bin/sh
# LX06 local KWS: private subscriber of continuous processed mono PCM.
#AUDIO-PATH: native-vpm
# Enrollment and scripts receive independent copies; never change official audio.
tpl=/usr/lib/lx06/wakeword/wakeword_template
reader=/usr/lib/lx06/xiaoai/processed-mic-read
supervisor=/usr/lib/lx06/wakeword/kws-supervisor
data=/data/wakeword
conf=$data/wakeword.conf
event=/tmp/lx06-ww-event
capture=--kws-framed
detect=detect
if [ "$#" = 1 ] && [ "$1" = --attest-only ]; then
    data=/data/wakeword/profiles/raw-slot4-hp65-g8-kwe2-v2
    [ "$(cat "$data/capture-domain" 2>/dev/null)" = raw-slot4-hp65-g8-kwe2-v2 ] || exit 2
    conf=$data/wakeword.conf
    event=/tmp/lx06-ww-attestation
    export LX06_WAKE_DATA="$data"
    export LX06_WAKE_FORENSICS_DIR=/tmp/lx06-wake-forensics-raw-hp65-g8-kwe2-v2
    capture=--kws-epoch
    detect=detect-epoch
elif [ "$#" != 0 ]; then
    exit 2
elif [ -f "${tpl%/*}/paired-capture-v1" ]; then
    capture=--kws-paired
    detect=detect-paired
fi
log_sink=/usr/bin/lx06-keeper
feature_id=$("$tpl" info 2>/dev/null | awk '$1=="FEATURE" {sub("model_version=", "", $3); id=$2 ":" $3; if($5~/^calibration=/){sub("calibration=", "", $5); id=id ":" $5} print id}')
work=/tmp/ww-watch-$$
supervisor_pid=
log_pid=
# Refuse a stale or pre-existing PID directory rather than reusing its FIFOs.
( umask 077; mkdir "$work" ) || exit 1
note() {
    printf '%s %s\n' "$(date '+%F %T')" "$1" >&6 || :
}
stop_children() {
    if [ -n "$supervisor_pid" ]; then
        # A FIFO command, never a delayed PID signal: the shell may already
        # have reaped its background job while processing detector events.
        printf Q >&9 2>/dev/null || :
        wait "$supervisor_pid" 2>/dev/null || :
        supervisor_pid=
        exec 9>&-
        rm -f "$work/control"
    fi
}
cleanup() {
    stop_children
    exec 6>&-
    [ -z "$log_pid" ] || wait "$log_pid" 2>/dev/null || :
    rm -f "$work/control" "$work/events" "$work/diagnostics" "$work/epoch-event" "$event.$$"
    exec 7<&- 8>&-
    rm -f "$work"/*.lwt
    rmdir "$work" 2>/dev/null
}
trap cleanup EXIT
launching=0
shutdown_pending=0
trap 'if [ "$launching" = 1 ]; then shutdown_pending=1; else exit 0; fi' INT TERM
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
[ -n "$feature_id" ] || { note 'feature identity unavailable; refusing uncalibrated detector'; exit 2; }
mkfifo "$work/events" || exit 1
# Match the Web enrollment policy byte-for-byte (U+4E00..U+9FFF), independent
# of the device locale. Legacy records remain in the configuration unchanged.
cjk3=$(printf '\344[\270-\277][\200-\277]|[\345-\351][\200-\277][\200-\277]')
active_name_ok() {
    printf '%s\n' "$1" | LC_ALL=C grep -Eq "^($cjk3){3,4}$"
}
retrain_missing() (
    exec 9>/tmp/lx06-ww-mutate.lock
    flock -n 9 || return
    [ -r "$conf" ] || return
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in word=*) ;; *) continue ;; esac
        name=""
        enabled=1
        calibrated_feature=
        migrate=0
        for kv in $(printf '%s' "$line" | tr '|' ' '); do
            case "$kv" in
                word=*) name="${kv#word=}" ;;
                enabled=*) enabled="${kv#enabled=}" ;;
                feature=*) calibrated_feature="${kv#feature=}" ;;
            esac
        done
        [ -n "$name" ] || continue
        active_name_ok "$name" || continue
        [ "$enabled" = "1" ] || continue
        m="$data/models/$name.lwt"
        if [ -s "$m" ]; then
            if "$tpl" check "$m" >/dev/null 2>&1; then
                # Unused. First boot of this version deletes non-norm words
                # instead of retraining them. Do not call this from the loop.
                if [ "$feature_id" = mfcc-lifter32:8:duration-bank2-candidate2-norm ] &&
                   { [ "$calibrated_feature" = mfcc-lifter32:8 ] ||
                     [ "$calibrated_feature" = mfcc-lifter32:8:duration-bank1 ] ||
                     [ "$calibrated_feature" = mfcc-lifter32:8:duration-bank2-candidate1 ]; }; then
                    migrate=1
                else continue; fi
            else
                # Quantization/features change score distributions. Never silently
                # rebuild a different model and attach its predecessor's threshold.
                [ -e "$data/models/$name.migration-required" ] || note "model incompatible: $name; retrain and calibrate threshold for $feature_id"
                touch "$data/models/$name.migration-required"
                continue
            fi
        fi
        recdir="$data/recs/$name"
        set --
        i=1
        while [ "$i" -le 10 ]; do
            [ -f "$recdir/$i.wav" ] && set -- "$@" "$recdir/$i.wav"
            i=$((i + 1))
        done
        [ "$#" -ge 3 ] || continue
        mkdir -p "$data/models" || continue
        failmark="$data/models/$name.trainfail"
        if [ -f "$failmark" ]; then
            newer=0
            for f in "$@"; do
                [ "$f" -nt "$failmark" ] && newer=1
            done
            [ "$newer" = "1" ] || continue
        fi
        [ "$migrate" = 0 ] || note "recalibrating $name for $feature_id; original recordings retained"
        if LX06_WW_LOCK_HELD=1 sh /usr/lib/lx06/wakeword/ww-train.sh "$name" >&6 2>&1 6>&-; then
            rm -f "$failmark"
            note "trained $name from $# recordings"
        else
            touch "$failmark"
            note "train failed $name"
        fi
    done < "$conf"
)

store=${tpl%/*}/ww_store
session_run=${reader%/*}/session-run
drain_epoch=0
if [ "$detect" != detect ] && "$supervisor" --epoch-drain-supported >/dev/null 2>&1; then
    drain_epoch=1
fi
drain_for_handoff() {
    [ "$drain_epoch" = 1 ] || return 0
    printf E >&9 || return 1
    drain=
    while IFS= read -r drain <&7; do
        case "$drain" in KWS_DRAINED|KWS_DRAIN_PENDING) break ;; esac
    done
    if [ "$drain" != KWS_DRAINED ]; then
        note 'detector drain incomplete at dispatch'
    fi
    rm -f "$work"/*.lwt
}
idle=0
while :; do
    if [ "$detect" != detect ] && [ -x "$session_run" ] && "$session_run" busy; then
        sleep 2 6>&-
        continue
    fi
    if [ "$detect" != detect-epoch ] && [ -x "$store" ] && [ -n "$feature_id" ]; then
        "$store" migrate "$data" "$feature_id" || note "local-wake wipe incomplete; official switch untouched"
    fi
    # Never auto-train, migrate, or write markers in the persistent store.
    rm -f "$work"/*.lwt
    set --
    if [ -r "$conf" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in word=*) ;; *) continue ;; esac
            name=; th=0.18; enabled=1; calibrated_feature=; calibrated_model=
            for kv in $(printf '%s' "$line" | tr '|' ' '); do
                case "$kv" in
                    word=*) name="${kv#word=}" ;;
                    threshold=*) th="${kv#threshold=}" ;;
                    enabled=*) enabled="${kv#enabled=}" ;;
                    feature=*) calibrated_feature="${kv#feature=}" ;;
                    model_sha256=*) calibrated_model="${kv#model_sha256=}" ;;
                esac
            done
            [ -n "$name" ] && [ "$enabled" = 1 ] || continue
            active_name_ok "$name" || continue
            [ "$calibrated_feature" = "$feature_id" ] || continue
            awk -v gate="$th" 'BEGIN {exit !(gate ~ /^[0-9]+([.][0-9]+)?$/ && gate+0 > 0 && gate+0 <= 0.26)}' || continue
            model="$data/models/$name.lwt"
            # Retraining in the same mode also changes the score distribution.
            # Missing bindings deliberately require compare/calibration again.
            [ "${#calibrated_model}" -eq 64 ] || continue
            # Hash and load the same bytes even if a trainer atomically
            # replaces the original between validation and detector startup.
            snapshot="$work/$name.lwt"
            cp "$model" "$snapshot" 2>/dev/null || continue
            model="$snapshot"
            actual_model=$(sha256sum "$model" 2>/dev/null | cut -d ' ' -f 1)
            [ "$actual_model" = "$calibrated_model" ] || continue
            [ -s "$model" ] && "$tpl" check "$model" >/dev/null 2>&1 && set -- "$@" "$name=$model:$th"
        done < "$conf"
    fi
    if [ "$#" -eq 0 ]; then
        [ "$idle" -eq 0 ] && note "idle: no enabled models with compatible calibration ($feature_id)"
        idle=1
        sleep 2 6>&-
        continue
    fi
    idle=0
    note "listen: $# model(s), independent processed stream"
    mkfifo "$work/control" || exit 1
    exec 9<>"$work/control"
    # Hold an events reader across asynchronous redirection startup. Otherwise
    # a shutdown trap can wait forever for a child blocked opening its stdout.
    exec 8<>"$work/events"
    # Defer traps until $! is saved; otherwise a signal between launch and
    # assignment can orphan the background process.
    launching=1
    ( printf 'KWS_READY\n'; GLIBC_TUNABLES=glibc.pthread.rseq=0 exec "$supervisor" "$reader" --name kws "$capture" -- "$tpl" "$detect" "$@" ) <&9 9>&- 8>&- > "$work/events" 2>&6 6>&- &
    supervisor_pid=$!
    launching=0
    [ "$shutdown_pending" = 0 ] || exit 0
    exec 7<"$work/events"
    IFS= read -r ready <&7
    exec 8>&-
    [ "$ready" = KWS_READY ] || { stop_children; exit 1; }
    while read -r kind label score source epoch first end available extra pair_end pair_stamp trailing; do
        case "$detect:$kind" in detect:WAKE|detect-paired:WAKE_PAIRED|detect-paired:WAKE_PAIRED_CLOUD|detect-paired:WAKE_PAIRED_LOCAL_NEAR|detect-paired:WAKE_PAIRED_LOCAL_NEAR_SEPARATED|detect-epoch:WAKE_EPOCH) ;; *) continue ;; esac
        # The detector's label must exactly match one of the validated model
        # arguments used for this detector instance, not the current config.
        loaded=0
        for spec in "$@"; do
            if [ "$label" = "${spec%%=*}" ]; then loaded=1; break; fi
        done
        [ "$loaded" = 1 ] || continue
        # Enrollment must never dispatch its own prompts/samples. Active script
        # ownership is checked centrally by webd before launching any action.
        if ! flock -n /tmp/lx06-ww-mutate.lock true; then continue; fi
        if [ "$detect" = detect-paired ]; then
            [ -n "$pair_stamp" ] || continue
            suffix=
            if [ "$kind" = WAKE_PAIRED_LOCAL_NEAR_SEPARATED ]; then
                case "$trailing" in ''|*[!0-9.]*) continue ;; esac
                suffix=" local-near-separated $trailing"
            else
                [ -z "$trailing" ] || continue
            fi
            [ "$kind" = WAKE_PAIRED_CLOUD ] && suffix=' cloud'
            [ "$kind" = WAKE_PAIRED_LOCAL_NEAR ] && suffix=' local-near'
            (umask 077; printf '%s %s %s %s %s %s %s %s %s %s%s\n' "$label" "$score" "$source" "$epoch" "$first" "$end" "$available" "$extra" "$pair_end" "$pair_stamp" "$suffix" > "$work/epoch-event") || continue
            drain_for_handoff || break
            mv -f "$work/epoch-event" "$event.paired"
            note "wake label=$label score=$score paired=1"
        elif [ "$detect" = detect-epoch ]; then
            [ -n "$available" ] && [ -z "$extra" ] || continue
            ( umask 077; printf '%s %s %s %s %s %s %s\n' "$label" "$score" "$source" "$epoch" "$first" "$end" "$available" > "$work/epoch-event" ) || continue
            drain_for_handoff || break
            mv -f "$work/epoch-event" "$event.epoch"
        else
            note "wake label=$label score=$score"
            logger -t lx06-wake "本地唤醒词命中：$label" 2>/dev/null || :
            printf '%s %s %s\n' "$label" "$score" "$(cut -d ' ' -f 1 /proc/uptime)" > "$event.$$" && mv -f "$event.$$" "$event"
        fi
    done <&7
    exec 7<&-
    stop_children
    note 'listen: stream ended; retry in 2s'
    sleep 2 6>&-
done
