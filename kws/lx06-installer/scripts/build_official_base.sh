#!/bin/sh
# Build-time: official stock 1.94.13 root.squashfs -> official/root.squashfs
# V13: single minimal patch = clear stale SSH enable markers right after /data
# is mounted (before the START=50 dropbear service runs). /data is persistent
# across re-flashes (LX06 USB flash writes system0 only), so a device
# previously flashed with an SSH variant would otherwise boot the stock
# dropbear with the old marker still set. Everything else stays stock.
# Idempotent: re-running does not double-insert.
# Args: $1=stock img (wsl path)  $2=out img (wsl path)
set -e
STOCK=$1; OUT=$2
WORK=/root/lx06-official
rm -rf "$WORK"; mkdir -p "$WORK"
unsquashfs -q -d "$WORK" "$STOCK"
BOOT="$WORK/etc/init.d/boot"
if ! grep -q 'rm -f /data/ssh_en /data/.ssh_en /tmp/ssh_en' "$BOOT"; then
  sed -i 's|^    mkdir -p /data/etc$|    rm -f /data/ssh_en /data/.ssh_en /tmp/ssh_en 2>/dev/null\n    mkdir -p /data/etc|' "$BOOT"
fi
grep -n 'rm -f /data/ssh_en' "$BOOT"
rm -f "$OUT"
mksquashfs "$WORK" "$OUT" -comp xz -b 131072 -all-root -always-use-fragments -no-xattrs -no-exports -no-recovery
rm -rf "$WORK"
echo "REBUILT_OK $OUT"
