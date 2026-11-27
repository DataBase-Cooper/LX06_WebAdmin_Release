#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
d=$(realpath "${1:?pass the frozen stock root filesystem}")
crt=$(realpath "${2:?pass the OEM glibc 2.25 CRT directory}")
test "$(sha256sum "$d/usr/lib/libxaudio_engine.so" | cut -d' ' -f1)" = ae3f7ca5502f74eb88d2001253cb0c3cd4fdb9d6e860c902b44964f0aa2dc65c
src=$here/epoch-enhancement.c
out=$(mktemp -d)
selection=${3:-all}
case $selection in all|history|four-mapped|speech|mono) ;; *) exit 2 ;; esac
if [ "$selection" = mono ]; then src=$here/single-ns.c; fi
trap 'rm -f "$out/enhancement.o" "$out/enhancement-probe" "$out/results"; rmdir "$out"' EXIT
cd "$out"
cc=arm-linux-gnueabihf-gcc
"$cc" -O2 -Wall -Wextra -Werror -marm -mcpu=cortex-a53 -mfpu=neon-vfpv4 -fPIE -c "$src" -o enhancement.o
"$cc" -nostdlib -pie -Wl,-E,--allow-shlib-undefined,--dynamic-linker,/lib/ld-linux-armhf.so.3 \
  "$crt/Scrt1.o" "$crt/crti.o" "$crt/crtbeginS.o" "$crt/elf-init.o" enhancement.o \
  "$d/usr/lib/libxaudio_engine.so" "$d/lib/libm.so.6" "$d/lib/libdl.so.2" "$d/lib/libc.so.6" -lgcc "$crt/crtendS.o" "$crt/crtn.o" -o enhancement-probe
unshare -m -n bash -eu -c '
    mount --make-rprivate /
    scratch=$(mktemp -d /tmp/lx06-afe-proof-XXXXXX)
    mount -t tmpfs -o size=48m,mode=700 tmpfs "$scratch"
    trap "umount \"$scratch\" && rmdir \"$scratch\"" EXIT
    mkdir -p "$scratch"/{lib,usr/lib,usr/share/xiaomi,tmp,proc}
    printf "processor : 0\nFeatures : half thumb fastmult vfp edsp neon vfpv3 tls vfpv4\n" > "$scratch/proc/cpuinfo"
    cp /usr/bin/qemu-arm-static "$scratch/qemu"
    cp "$2/enhancement-probe" "$scratch/probe"
    for lib in ld-linux-armhf.so.3 libc.so.6 libgcc_s.so.1 libm.so.6 libdl.so.2; do cp -L "$1/lib/$lib" "$scratch/lib/$lib"; done
    cp -L "$1/usr/lib/libstdc++.so.6" "$1/usr/lib/libxaudio_engine.so" "$scratch/usr/lib/"
    cp -a "$1/usr/share/xiaomi/dnn_vad_model" "$scratch/usr/share/xiaomi/"
    modes="enabled bypass enabled recreate four-fresh four-recreate history"
    if [ "$3" != all ]; then modes=$3; fi
    if [ "$3" = speech ] || [ "$3" = mono ]; then
      if [ "$3" = speech ]; then modes="six-speech four-speech"; fi
      ffmpeg -nostdin -loglevel error -f lavfi -i "flite=text=Hello speaker. Please tell me the time. Hello speaker. Please tell me the time again.:voice=slt" -af apad -t 6 -ac 1 -ar 16000 -f s16le "$scratch/speech.pcm"
    fi
    for mode in $modes; do
      chroot "$scratch" /qemu -E LD_LIBRARY_PATH=/usr/lib:/lib -E LD_PRELOAD=libstdc++.so.6:libm.so.6 /probe "$mode"
    done
' bash "$d" "$out" "$selection" | tee "$out/results"
if [ "$selection" != all ]; then exit 0; fi
awk '
BEGIN {split("enabled bypass enabled old-source new-source four-fresh four-old four-new",modes)}
/^mode=/ {
  n++
  if ($1 != ("mode=" modes[n]) ||
      $2 != "calls=750" || $3 != "aliases=0" || $4 != "packets=600" ||
      $7 !~ /^hash=[0-9a-f]+$/) bad=1
  # This incomplete-array control produces silence with this frozen library.
  if (n >= 6 && ($5 != "noise_rms=0.00" || $6 != "near_mix_rms=0.00" ||
                 $7 != "hash=9ebf9a6ec921bb25")) bad=1
  hash[n]=$7
}
END {exit bad || n != 8 || hash[1] != hash[3] || hash[1] == hash[2] ||
          hash[4] == hash[5] || hash[1] != hash[5] ||
          hash[6] != hash[8] || hash[6] != hash[7] || hash[6] == hash[1]}
' "$out/results"
