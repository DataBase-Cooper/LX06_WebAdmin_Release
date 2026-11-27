#!/bin/sh
# Enrollment ASR. Always runs. Does not read LX06-WAKE-CLOUD-VERIFY.
#
# The finished wav is sent in one burst through wake_cloud_asr, the same probe
# client the gray-zone wake check uses. This is not a control_asr listen-window
# reader and does not stay attached. control_asr remains mounted only for the
# post-interrupt listen window of a script that declared control phrases.
#
# Wake-time verify stays at 800ms and 1.8s. Enrollment may send up to 2.5s and
# wait 2500ms because the user is still on the recording screen.
# A failure still leaves the wav; the sidecar says the sample needs re-record.
# The recognized text is stored in the sidecar only, not in the wake log.
wav=$1
out=$2
base=${LX06_WW_BASE:-/usr/lib/lx06/wakeword}
helper=${LX06_WAKE_CLOUD_HELPER:-$base/wake_cloud_asr}
commit=${LX06_WAKE_SAMPLE_COMMIT:-$base/wake_sample_commit}
sock=${LX06_WAKE_CLOUD_SOCKET:-/tmp/lx06-control-asr-probe.sock}
budget=2500
max_samples=40000

write_status() {
    printf '%s\n' "$2" | "$commit" "$out" "$1"
}

[ -n "$wav" ] && [ -n "$out" ] && [ -x "$commit" ] || exit 1
if [ ! -f "$wav" ]; then
    write_status unavailable ""
    exit 0
fi
size=$(wc -c < "$wav")
magic=$(dd if="$wav" bs=1 count=4 2>/dev/null)
if [ "$magic" != "RIFF" ]; then
    write_status unavailable ""
    exit 0
fi
pcm_bytes=$((size - 44))
samples=$((pcm_bytes / 2))
if [ "$samples" -lt 8000 ]; then
    write_status too_short ""
    exit 0
fi
if [ "$samples" -gt "$max_samples" ]; then
    write_status too_long ""
    exit 0
fi
if [ ! -x "$helper" ]; then
    write_status unavailable ""
    exit 0
fi
text=$(dd if="$wav" bs=1 skip=44 2>/dev/null | "$helper" "$sock" "$budget" "$max_samples")
rc=$?
printf '%s\n' "$text" | "$commit" "$out" "$rc"
exit 0
