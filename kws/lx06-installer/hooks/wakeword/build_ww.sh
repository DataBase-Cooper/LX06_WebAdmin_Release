#!/bin/sh
# 交叉编译 LX06 唤醒引擎（WSL armhf）。产物: build/wakeword_template
# Embedding 权重编进二进制，开机由 init-launch 拷到 /data/lx06/wakeword/。
set -e
cd "$(dirname "$0")"
mkdir -p build
SRC=../../../lwake-c/src
arm-linux-gnueabihf-gcc -O2 -static -ffast-math -DLWAKE_MAX_S=51200 -I"$SRC" -o build/wakeword_template \
  wakeword_template.c "$SRC/lwake.c" "$SRC/lwake_mel.c" "$SRC/lwake_cnn.c" "$SRC/lwake_tables.c" -lm
arm-linux-gnueabihf-strip build/wakeword_template 2>/dev/null || true
file build/wakeword_template
ls -la build/wakeword_template
echo BUILD-OK
