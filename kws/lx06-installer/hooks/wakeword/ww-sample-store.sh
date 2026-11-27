#!/bin/sh
# T02 private collection only. Called with freshly captured audio, never by a
# web upload/copy endpoint. Enrollment and calibration cannot read this tree.
set -eu
umask 077
root=$1
word=$2
purpose=$3
condition=$4
capture=$5
case "$purpose" in acceptance|calibration|diagnostic) ;; *) exit 2 ;; esac
case "$condition" in normal|far|soft) ;; *) exit 2 ;; esac
case "$word" in ''|*'/'*|*'\'*|*'"'*|*'..'*|*'
'*|*'|'*) exit 2 ;; esac
folder=$root/samples/$purpose/$word
mkdir -p "$folder"
chmod 700 "$root/samples" "$root/samples/$purpose" "$folder"
# Bound persistent collection to 100 clips per word/purpose (30 are required
# for acceptance). Nothing is automatically evicted or relabelled.
count=0
for entry in "$folder"/s-*; do [ ! -d "$entry" ] || count=$((count+1)); done
[ "$count" -lt 100 ] || { echo '独立样本已达100条，请先导出'; exit 1; }
slot=$(mktemp -d "$folder/s-XXXXXX")
trap 'rm -f "$slot/audio.wav" "$slot/sample.json" "$slot/ready"; rmdir "$slot" 2>/dev/null || true' EXIT
cp "$capture" "$slot/audio.wav"
digest=$(sha256sum "$slot/audio.wav")
digest=${digest%% *}
[ "${#digest}" -eq 64 ] || exit 1
id=${slot##*/}
printf '{"id":"%s","label":"%s","path":"%s/%s/audio.wav","sha256":"%s","purpose":"%s","origin":"human","condition":"%s"}\n' \
    "$id" "$word" "$word" "$id" "$digest" "$purpose" "$condition" > "$slot/sample.json"
chmod 600 "$slot/audio.wav" "$slot/sample.json"
# Presence of ready is the publication point. Partial captures never list.
touch "$slot/ready"
trap - EXIT
printf 'sample=%s purpose=%s condition=%s sha256=%s' "$id" "$purpose" "$condition" "$digest"
