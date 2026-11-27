/* glibc225_shim.c — 工具链 glibc 2.41 头文件在 armhf(time64) 下生成
 * __clock_gettime64/__time64/__nanosleep64/__localtime64_r/__fcntl_time64
 * 调用，设备 glibc 2.25（kernel 4.9.61）只有 32 位老符号（time64 syscall
 * 需 kernel ≥5.1；静态 2.41 二进制实测卡 syscall 403=b+403 clock_gettime64
 * bad_syscall 循环，dmesg 每 0.5s 一次 oops）。本 shim 把这些符号映射回
 * 老 32 位 syscall / 老符号。仅当 --dynamic-linker 指向设备 ld 2.25 时链接。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/syscall.h>

struct t32 { int tv_sec; long tv_nsec; };
struct ts64 { long long tv_sec; long tv_nsec; }; /* ≡ glibc __timespec64 布局 */

/* ARM32 EABI syscall 号（unistd-eabi.h 实证，228=fsetxattr/263=clock_gettime，
 * 曾误用 x86 习惯号 228 导致设备 mic 时间全乱 + qemu SIGILL） */
#define ARM_NR_clock_gettime 263
#define ARM_NR_time          13
#define ARM_NR_nanosleep     162
#define ARM_NR_fcntl         55
#define ARM_NR_fstatat64     327

int __clock_gettime64(clockid_t c, struct ts64 *ts)
{
    struct t32 t;
    long r = syscall(ARM_NR_clock_gettime, c, &t);
    if (r < 0) return -1;
    ts->tv_sec = t.tv_sec;
    ts->tv_nsec = t.tv_nsec;
    return 0;
}

/* 调用端(2.41 头)声明 __time64 返回 time_t(64 位, r0:r1)；shim 若声明 int 则
 * 只写 r0，调用端读 r1=残值（实测高位 0xffda4798 垃圾）→ 必须 time_t 返回 */
time_t __time64(time_t *t)
{
    /* 设备内核 EABI 的 time(13) 不可靠（ENOSYS），统一走 clock_gettime(CLOCK_REALTIME)。
     * ARM EABI 标准映射: 0=REALTIME 1=MONOTONIC（实测：settimeofday 只改 id=0）。
     * 32 位内核只写 8 字节 {int,int}，必须用 t32 读，否则 64 位字段高半是垃圾 */
    struct t32 t32ts;
    long r = syscall(ARM_NR_clock_gettime, 0, &t32ts);
    if (r < 0) return (time_t)-1;
    if (t) *t = (time_t)t32ts.tv_sec;
    return (time_t)t32ts.tv_sec;
}

int __nanosleep64(const struct ts64 *req, struct ts64 *rem)
{
    struct t32 rq = { (int)req->tv_sec, req->tv_nsec }, rm;
    long r = syscall(ARM_NR_nanosleep, &rq, &rm);
    if (r < 0) return -1;
    if (rem) { rem->tv_sec = rm.tv_sec; rem->tv_nsec = rm.tv_nsec; }
    return 0;
}

int __stat64_time64(const char *path, void *st)
{
    /* 必须显式传第4参 flags=0：老 fstatat64 取 4 个参数，r3 残留垃圾位 → EINVAL（间歇性！） */
    long r = syscall(ARM_NR_fstatat64, AT_FDCWD, path, st, (int)0);
    /* glibc 2.25 的 stat64 = kernel stat64 布局；fstatat64(AT_FDCWD,...) 直调 */
    return (int)r;
}

/* 2.25 libc 的 localtime_r 是 32 位；2.41 头下直接写 localtime_r 会再解析到
 * __localtime64_r（递归），故用 asm 标签钉死老符号。 */
/* glibc 2.25 的 localtime_r 是 2 参 (const time_t*, struct tm*)，time_t=int */
static void *old_localtime_r(const int *t, struct tm *tmv)
    __asm__("localtime_r");
int __localtime64_r(const time_t *t, struct tm *tmv, char *buf, size_t l)
{
    int t32 = (int)*t;
    old_localtime_r(&t32, tmv);
    return 0;
}

/* glibc 2.41 的 fcntl() 宏对部分 cmd 路由到 __fcntl_time64(varargs)。
 * 我们只用 F_SETFL/F_GETFL（int 参数）→ 老 SYS_fcntl(55) 足够。 */
int __fcntl_time64(int fd, int cmd, ...)
{
    va_list ap;
    long a = 0;
    va_start(ap, cmd);
    a = va_arg(ap, long);
    va_end(ap);
    return (int)syscall(ARM_NR_fcntl, fd, cmd, a);
}
