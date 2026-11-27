#!/usr/bin/env bash
# 只编译原生 keeper，不生成镜像/刷机包，不连接设备。必须显式给出输出路径。
# 例：CC=arm-linux-gnueabihf-gcc bash scripts/build_keeper.sh <scratch>/lx06-keeper
# 独立静态 ARM32 ELF，避免 2.41 开发头文件与设备 glibc 2.25 的动态 ABI 混用。
# 依赖交叉工具链静态库 libc.a（/usr/arm-linux-gnueabihf/lib/）。2026-10-21 实证：
# 为编 xiaoai-agent（动态，需设备 2.25 运行库）曾把该目录换成 2.25 .so 集
# （只有 .so 无 .a，静态链接报 ld: cannot find -lc），现 2.25 集存于
# lib-225-xiaoai/；静态编译（keeper/xaudio-tap）前确认 lib/ 是含 libc.a 的 2.41 集。
set -euo pipefail
[ "$#" -eq 1 ] || { printf 'usage: %s <output-binary>\n' "$0" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-arm-linux-gnueabihf-gcc}
OUT=$1
mkdir -p "$(dirname "$OUT")"
"$CC" -std=gnu99 -Os -static -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 \
    -Wall -Wextra -Werror -fstack-protector-strong \
    -o "$OUT" "$REPO/hooks/robust/keeper.c"
file "$OUT"
