#!/bin/sh
# z10: 交叉编译 LX06 唤醒引擎 INT8 版（WSL armhf）。
# 逐通道对称 int8 权重表；分数有漂移，必须独立训练/compare/校准阈值。
set -e
cd "$(dirname "$0")"
mkdir -p build
SRC=../../../lwake-c/src
CC=${CC:-arm-linux-gnueabihf-gcc}
$CC -O3 -mcpu=cortex-a53 -mfpu=neon-vfpv4 -static -ffast-math -DWW_EMBED_INT8=1 -DLWAKE_MAX_S=51200 -I"$SRC" -o build/wakeword_template_int8 \
  wakeword_template.c "$SRC/lwake.c" "$SRC/lwake_mel.c" "$SRC/lwake_cnn_int8.c" "$SRC/lwake_tables_int8.c" -lm
arm-linux-gnueabihf-strip build/wakeword_template_int8 2>/dev/null || true
file build/wakeword_template_int8
ls -la build/wakeword_template build/wakeword_template_int8 2>/dev/null | awk '{print $5, $9}'
echo BUILD-INT8-OK
