#!/usr/bin/env bash
# Fresh ROM helpers only. Provider URLs, login rules and UI declarations stay in
# uploadable scripts; never reuse helper binaries inherited from an old image.
set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 1 ]; then
  echo 'usage: build_model_adapter.sh OUTPUT_DIR' >&2
  exit 2
fi
repo=$(cd "$(dirname "$0")/.." && pwd)
: "${LX06_LIB:?stock firmware lib directory required (libc.so.6, libgcc_s.so.1)}"
: "${LX06_LEGACY_LIB:?stock 2.25 ABI directory required (sysarm-* filenames)}"
: "${OPUS_LIBRARY:?stock-compatible ARM libopus file required}"

for command in arm-linux-gnueabihf-gcc arm-linux-gnueabihf-strip arm-linux-gnueabihf-readelf install stat sha256sum; do
  command -v "$command" >/dev/null || { echo "missing build tool: $command" >&2; exit 1; }
done
for file in "$LX06_LIB/libc.so.6" "$LX06_LIB/libgcc_s.so.1" \
            "$LX06_LEGACY_LIB/sysarm-libc-2.25.so" \
            "$LX06_LEGACY_LIB/sysarm-libcrypto.so.1.0.0" \
            "$LX06_LEGACY_LIB/sysarm-libdl.so.2" \
            "$LX06_LEGACY_LIB/sysarm-libgcc_s.so.1" \
            "$LX06_LEGACY_LIB/sysarm-libm-2.25.so" "$OPUS_LIBRARY"; do
  [ -f "$file" ] && [ -r "$file" ] || { echo "missing readable ABI input: $file" >&2; exit 1; }
done
if ! printf '#include <opus/opus.h>\n' | arm-linux-gnueabihf-gcc -E -x c - >/dev/null; then
  echo 'ARM compiler sysroot needs Opus development headers (opus/opus.h); OPUS_LIBRARY supplies only the link library' >&2
  exit 1
fi
# The existing per-helper builder changes directory. Resolve all external input
# paths first, so a caller's relative paths cannot silently target other files.
LX06_LIB=$(cd "$LX06_LIB" && pwd)
LX06_LEGACY_LIB=$(cd "$LX06_LEGACY_LIB" && pwd)
OPUS_LIBRARY=$(cd "$(dirname "$OPUS_LIBRARY")" && pwd)/$(basename "$OPUS_LIBRARY")
export LX06_LIB LX06_LEGACY_LIB OPUS_LIBRARY
mkdir -p "$1"
output=$(cd "$1" && pwd)
stage=$(mktemp -d "$output/.model-adapter.XXXXXX")
trap 'rm -rf "$stage"' EXIT

# SquashFS staging must preserve Unix executable modes. A default Windows
# DrvFS mount may report 0777 even after chmod; fail before compiling there.
touch "$stage/mode-check"
chmod 755 "$stage/mode-check"
if [ "$(stat -c '%a' "$stage/mode-check")" != 755 ]; then
  echo 'OUTPUT_DIR must preserve Unix permissions; use a Linux filesystem or a metadata-enabled mount' >&2
  exit 1
fi
rm "$stage/mode-check"

check_abi() {
  local binary=$1
  arm-linux-gnueabihf-readelf -h "$binary" > "$stage/header"
  grep -Eq 'Class:[[:space:]]+ELF32' "$stage/header" &&
    grep -Eq 'Machine:[[:space:]]+ARM' "$stage/header" &&
    grep -Eq 'Flags:.*hard-float ABI' "$stage/header" || {
      echo "not an ARM hard-float ELF32 helper: $binary" >&2; return 1;
    }
  arm-linux-gnueabihf-readelf -l "$binary" > "$stage/programs"
  grep -Fq 'Requesting program interpreter: /lib/ld-linux-armhf.so.3' "$stage/programs" || {
    echo "unexpected dynamic interpreter: $binary" >&2; return 1;
  }
  arm-linux-gnueabihf-readelf --dyn-syms -W "$binary" > "$stage/symbols"
  # Read the whole symbol table rather than a grep -q pipe: with pipefail a
  # closed early reader must not turn the ABI rejection into a false success.
  if ! awk '
    /@GLIBC_/ {
      for (i=1; i<=NF; i++) if (match($i, /@GLIBC_[0-9]+\.[0-9]+/)) {
        version=substr($i, RSTART+7, RLENGTH-7); split(version, parts, ".");
        if (parts[1]>2 || (parts[1]==2 && parts[2]>25)) bad=1;
      }
    }
    /UND/ && /(__isoc23_|__[^[:space:]]*time64)/ { bad=1 }
    END { exit bad ? 1 : 0 }
  ' "$stage/symbols"; then
    echo "helper requires symbols newer than stock glibc 2.25: $binary" >&2
    return 1
  fi
}

helpers=(model-tool model-audio model-asr model-voice model-http script-settings)
for helper in "${helpers[@]}"; do
  if [ "$helper" = script-settings ]; then
    CC=arm-linux-gnueabihf-gcc bash "$repo/scripts/build_script_settings.sh" "$stage/$helper"
  else
    bash "$repo/runtime/model-adapter/build.sh" "$stage/$helper" "$helper.c"
  fi
  check_abi "$stage/$helper"
done
# Publish only after all six fresh builds have passed. Each caller owns its
# image replacement allowlist and must verify the final packed image too.
for helper in "${helpers[@]}"; do
  install -m 755 "$stage/$helper" "$output/$helper"
  [ "$(stat -c '%a' "$output/$helper")" = 755 ] || { echo "incorrect helper mode: $helper" >&2; exit 1; }
done
for helper in "${helpers[@]}"; do sha256sum "$output/$helper"; done
