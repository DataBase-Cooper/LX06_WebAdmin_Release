/* shim225.c —— glibc 2.41 头文件产物 → 设备 glibc 2.25 符号桥。
 *
 * 背景：.o 用 2.41 交叉头文件编译（_GNU_SOURCE 下），部分符号名被
 * __REDIRECT 改名为 2.41 专属符号，设备 2.25 库中不存在：
 *   __isoc23_strtol   / __isoc23_strtoul   （2.41 把 strtol/strtoul 重定向）
 *   __isoc23_strtoll  （2.41 把 strtoll 重定向；2026-11-02 upgrade status 新增）
 *   __isoc23_fscanf   （2.41 把 fscanf 重定向）
 *   __isoc23_sscanf   （2.41 把 sscanf 重定向；2026-10-14 h_systemstats 新增）
 * 2.25 库中实际存在的等价符号：strtol@GLIBC_2.4(W) strtoul@GLIBC_2.4(W)
 *                              __isoc99_vfscanf@GLIBC_2.7(T)
 *                              fmemopen@GLIBC_2.1（sscanf 桥用）
 * stat 单独处理（webd.c 内用 __xstat 宏，见 LX06_STAT），不在此 shim。
 *
 * 本 shim 定义强符号，链接期直接满足引用。
 *
 * 关键坑（2026-10-10 定位，真机 O2 死循环根因）：本文件若 #define _GNU_SOURCE，
 * 函数体内对 strtol()/strtoul() 的调用会被 2.41 头同样 __REDIRECT 成
 * __isoc23_strtol → 函数体退化为 b.w 自身 → 无限递归（O0 侥幸能跑，O2 必死）。
 * 故不定义 _GNU_SOURCE：strtol/strtoul 保持基础符号名，2.25 libc 的
 * strtol@GLIBC_2.4 可解析。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

long int __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}
long long int __isoc23_strtoll(const char *nptr, char **endptr, int base)
{
    return strtoll(nptr, endptr, base);
}
unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul(nptr, endptr, base);
}

/* fscanf：2.25 有 __isoc99_vfscanf（FILE* 版变参核心），用它实现 */
int __isoc99_vfscanf(FILE *stream, const char *format, va_list ap);
int __isoc23_fscanf(FILE *stream, const char *format, ...)
{
    va_list ap;
    int r;
    va_start(ap, format);
    r = __isoc99_vfscanf(stream, format, ap);
    va_end(ap);
    return r;
}

/* sscanf：走 fmemopen（2.25 有，GLIBC_2.1）把字符串当文件，
 * 复用同一个 __isoc99_vfscanf 核心。str/fclose 保持基础符号名（见文件头坑）。 */
int __isoc23_sscanf(const char *str, const char *format, ...)
{
    FILE *f;
    va_list ap;
    int r;
    if (!str || !format) return -1;
    f = fmemopen((void *)str, __builtin_strlen(str), "r");
    if (!f) return -1;
    va_start(ap, format);
    r = __isoc99_vfscanf(f, format, ap);
    va_end(ap);
    fclose(f);
    return r;
}
