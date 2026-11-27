#!/bin/sh
set -eu
cd "$(dirname "$0")"
SRC=../../../lwake-c/src
CC=${CC:-gcc}
case "${1:-}" in
  fp32) out=/tmp/ww-fp32-host; mode=-DWW_EMBED_INT8=0
    set -- "$SRC/lwake.c" "$SRC/lwake_mel.c" "$SRC/lwake_cnn.c" "$SRC/lwake_tables.c" ;;
  int8) out=/tmp/ww-host; mode=-DWW_EMBED_INT8=1
    set -- "$SRC/lwake.c" "$SRC/lwake_mel.c" "$SRC/lwake_cnn_int8.c" "$SRC/lwake_tables_int8.c" ;;
  mfcc) out=/tmp/ww-mfcc-host; mode=-DWW_MFCC=1; set -- ;;
  mfcc-robust) out=/tmp/ww-mfcc-robust-host; mode=-DWW_MFCC=3; set -- ;;
  *) echo 'usage: build_ww_host.sh fp32|int8|mfcc|mfcc-robust' >&2; exit 2 ;;
esac
$CC -O3 -ffast-math "$mode" -DLWAKE_MAX_S=51200 -I"$SRC" \
  -o "$out" wakeword_template.c "$@" -lm
"$out" info
