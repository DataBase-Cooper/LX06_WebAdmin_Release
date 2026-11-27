/* stat225_shim.c —— glibc 2.41 头文件产物 → 设备 glibc 2.25 的 stat 符号桥。
 *
 * 背景：本组件的 .o 用 2.41 交叉头文件、_FILE_OFFSET_BITS=32（不带
 * __USE_FILE_OFFSET64）编译，sys/stat.h 在这种配置下把 stat/fstat/lstat/fstatat
 * 声明为普通符号（无 __REDIRECT、无宏），对象因此直接引用裸符号
 * stat / fstat / lstat / fstatat；而设备 OEM glibc 2.25 的 libc 只导出
 * __xstat / __fxstat / __lxstat / __fxstatat（及其 *64，均 @@GLIBC_2.4），
 * 没有裸符号 → 动态链接失败（-Wl,-z,defs）。
 * 与 shim225.c 的 __isoc23_* 重定向是同类问题但符号不同；shim225.c 头注释
 * 已声明「stat 单独处理，不在此 shim」，故 stat 桥独立成此文件，
 * 不塞进 time64 的 glibc225_shim.c。
 *
 * 布局安全（为什么必须走非 64 位接口）：ARM 上 struct stat64 比 struct stat
 * 大（88B vs 104B）。调用点（webd 的 wake_p05_api.h / wake_collection_api.h、
 * processed-mic 经 script-policy.h 的 LX06_POLICY_FSTAT 默认 fstat、
 * official-router.c、lx06-listen-window.c 等）的缓冲区类型是 2.41 头下的
 * struct stat（= 2.25 原生 88B 布局）。若本 shim 调 __xstat64/__fxstat64/
 * __lxstat64/__fxstatat64，64 位结果会写进较小的 struct stat 缓冲区造成溢出，
 * 因此这里一律映射到非 64 位的 __xstat 族，_STAT_VER = 3（与 webd.c 的
 * LX06_STAT/LX06_FSTAT/LX06_STATAT 宏使用的版本号一致）。
 * 本 shim 禁止调用 __xstat64 / __fxstat64 / __lxstat64 / __fxstatat64。
 *
 * 不改动：webd.c 的 LX06_STATBUF/LX06_STAT/LX06_FSTAT/LX06_STATAT
 *（struct stat64 + __xstat64）与 session-owner.h 的 LX06_SESSION_STAT*
 * 继续走 64 位路径，本 shim 不影响它们。
 *
 * Compile separately with POSIX 2008, time32/off32. GNU feature exposure
 * alone does not cause stat recursion; the historic bug concerns C23 numeric
 * wrappers. Reject large-file/time redirects explicitly below.
 */
/* Refuse incompatible caller options before the pinned native-only header
 * subset can try to include an off64/time64 helper that it does not supply. */
#if (defined(_TIME_BITS) && _TIME_BITS != 32) || \
    (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS != 32)
#error "stat225 requires native time32/off32, without stat redirects"
#endif
#if !defined(__arm__) || !defined(__ARM_PCS_VFP)
#error "stat225 requires ARM hard-float glibc"
#endif
#include <sys/stat.h>
#include <stddef.h>

/* Review correction: native ARM stat is 88B, alignment 8 (not 96B).
 * GNU feature exposure alone is harmless here; large-file/time redirects are
 * not. #undef cannot undo an asm-level __REDIRECT, so reject those ABIs.
 * See sysroot/oem-glibc-2.25/README.md for upstream and OEM binary evidence.
 */
#if !defined(__arm__) || !defined(__ARM_PCS_VFP) || !defined(__GLIBC__)
#error "stat225 requires ARM hard-float glibc"
#endif
#if !__GLIBC_PREREQ(2, 33)
#error "legacy headers already provide xstat wrappers; omit stat225 shim"
#endif
#if defined(__USE_FILE_OFFSET64) || defined(__USE_TIME_BITS64) || \
    (defined(_TIME_BITS) && _TIME_BITS != 32) || \
    (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS != 32)
#error "stat225 requires native time32/off32, without stat redirects"
#endif
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#error "stat225 requires POSIX 2008 declarations"
#endif

_Static_assert(sizeof(struct stat) == 88, "OEM stat size");
_Static_assert(_Alignof(struct stat) == 8, "OEM stat alignment");
#define LX06_STAT_FIELD(field, offset, width) \
    _Static_assert(offsetof(struct stat, field) == offset && \
                   sizeof(((struct stat *)0)->field) == width, "OEM " #field)
LX06_STAT_FIELD(st_dev, 0, 8);
LX06_STAT_FIELD(st_ino, 12, 4);
LX06_STAT_FIELD(st_mode, 16, 4);
LX06_STAT_FIELD(st_nlink, 20, 4);
LX06_STAT_FIELD(st_uid, 24, 4);
LX06_STAT_FIELD(st_gid, 28, 4);
LX06_STAT_FIELD(st_rdev, 32, 8);
LX06_STAT_FIELD(st_size, 44, 4);
LX06_STAT_FIELD(st_blksize, 48, 4);
LX06_STAT_FIELD(st_blocks, 52, 4);
LX06_STAT_FIELD(st_atim, 56, 8);
LX06_STAT_FIELD(st_mtim, 64, 8);
LX06_STAT_FIELD(st_ctim, 72, 8);
_Static_assert(sizeof(((struct stat *)0)->st_atim.tv_sec) == 4, "OEM time32");
#undef LX06_STAT_FIELD

enum { LX06_STAT_VER = 3 };

#undef stat
#undef fstat
#undef lstat
#undef fstatat
#undef __xstat
#undef __fxstat
#undef __lxstat
#undef __fxstatat

extern int __xstat(int __statver, const char *__path, struct stat *__buf);
extern int __fxstat(int __statver, int __fd, struct stat *__buf);
extern int __lxstat(int __statver, const char *__path, struct stat *__buf);
extern int __fxstatat(int __statver, int __fd, const char *__path,
                      struct stat *__buf, int __flags);

/* Local binding also prevents this DSO from interposing an unrelated libc's
 * stat ABI. No work after the OEM call: preserve flags, return value and errno.
 */
__attribute__((visibility("hidden")))
int stat(const char *path, struct stat *stbuf)
{
    return __xstat(LX06_STAT_VER, path, stbuf);
}

__attribute__((visibility("hidden")))
int fstat(int fd, struct stat *stbuf)
{
    return __fxstat(LX06_STAT_VER, fd, stbuf);
}

__attribute__((visibility("hidden")))
int lstat(const char *path, struct stat *stbuf)
{
    return __lxstat(LX06_STAT_VER, path, stbuf);
}

__attribute__((visibility("hidden")))
int fstatat(int fd, const char *path, struct stat *stbuf, int flags)
{
    return __fxstatat(LX06_STAT_VER, fd, path, stbuf, flags);
}
