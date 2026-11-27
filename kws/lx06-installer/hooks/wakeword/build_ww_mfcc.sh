#!/bin/sh
# Experimental local-wake compatible MFCC build; never replaces the active engine.
set -eu
cd "$(dirname "$0")"
mkdir -p build
mode=1
output=build/wakeword_template_mfcc
if [ "${1:-}" = robust ]; then mode=3; output=build/wakeword_template_mfcc_robust; fi
CC=${CC:-arm-linux-gnueabihf-gcc}
# Joint calibration compares recomputed enrollment features byte-for-byte.
# Both ARM programs must compile their shared MFCC code at the same level.
mfcc_optimization=-O3
"$CC" "$mfcc_optimization" -mcpu=cortex-a53 -mfpu=neon-vfpv4 \
  -static -ffast-math -DWW_MFCC=$mode -o "$output" wakeword_template.c -lm
arm-linux-gnueabihf-strip "$output"
file "$output"
"$CC" -O2 -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static -DWW_STORE_TOOL \
  -o build/ww_store ww_store.c
arm-linux-gnueabihf-strip build/ww_store
file build/ww_store
if [ "$mode" = 3 ]; then
  "$CC" "$mfcc_optimization" -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static -ffast-math \
    -o build/wakeword_calibrate wakeword_calibrate.c -lm
  arm-linux-gnueabihf-strip build/wakeword_calibrate
  file build/wakeword_calibrate
  # Same framed probe client as hooks/xiaoai/control-asr-probe-client.c.
  # OPUS_A is a static ARM libopus.a; OPUS_INC contains opus/opus.h.
  # No device credential is linked. Without those paths the helper is skipped.
  if [ -n "${OPUS_A:-}" ] && [ -n "${OPUS_INC:-}" ]; then
    "$CC" -O2 -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static \
      -I"$OPUS_INC" -I../../runtime/doubao/src \
      -o build/wake_cloud_asr wake_cloud_asr.c ../../runtime/doubao/src/json.c \
      "$OPUS_A" -lm
    arm-linux-gnueabihf-strip build/wake_cloud_asr
    file build/wake_cloud_asr
  else
    echo "wake_cloud_asr skipped: set OPUS_A and OPUS_INC to a static ARM libopus" >&2
  fi
  # Enrollment sidecar writer. No libopus and no glibc newer than the static sysroot.
  "$CC" -O2 -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static \
    -DWAKE_SAMPLE_TOOL -o build/wake_sample_commit wake_sample.c wake_pinyin.c
  arm-linux-gnueabihf-strip build/wake_sample_commit
  file build/wake_sample_commit
fi
