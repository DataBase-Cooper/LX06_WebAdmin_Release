/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#ifndef DROPBEAR_INCLUDES_H_
#define DROPBEAR_INCLUDES_H_


#include "config.h"
#include "options.h"
#include "debug.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/param.h> /* required for BSD4_4 define */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <syslog.h>
#include <netdb.h>
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <time.h>

#ifdef HAVE_UTMP_H
#include <utmp.h>
#endif

#ifdef HAVE_UTMPX_H
#include <utmpx.h>
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifdef HAVE_LASTLOG_H
#include <lastlog.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include <arpa/inet.h>

/* netbsd 1.6 needs this to be included before netinet/ip.h for some
 * undocumented reason */
#ifdef HAVE_NETINET_IN_SYSTM_H
#include <netinet/in_systm.h>
#endif

#include <netinet/ip.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifndef DISABLE_ZLIB
#include <zlib.h>
#endif

#ifdef HAVE_UTIL_H
#include <util.h>
#endif

#ifdef HAVE_SHADOW_H
#include <shadow.h>
#endif

#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#ifdef BUNDLED_LIBTOM
#include "libtomcrypt/src/headers/tomcrypt.h"
#include "libtommath/tommath.h"
#else
#include <tomcrypt.h>
#include <tommath.h>
#endif


#include "compat.h"

#ifndef HAVE_U_INT8_T
typedef unsigned char u_int8_t;
#endif /* HAVE_U_INT8_T */
#ifndef HAVE_UINT8_T
typedef u_int8_t uint8_t;
#endif /* HAVE_UINT8_T */

#ifndef HAVE_U_INT16_T
typedef unsigned short u_int16_t;
#endif /* HAVE_U_INT16_T */
#ifndef HAVE_UINT16_T
typedef u_int16_t uint16_t;
#endif /* HAVE_UINT16_T */

#ifndef HAVE_U_INT32_T
typedef unsigned int u_int32_t;
#endif /* HAVE_U_INT32_T */
#ifndef HAVE_UINT32_T
typedef u_int32_t uint32_t;
#endif /* HAVE_UINT32_T */

#ifdef SO_PRIORITY
#include <linux/types.h>
#include <linux/pkt_sched.h>
#endif

#include "fake-rfc2553.h"

#ifndef LOG_AUTHPRIV
#define LOG_AUTHPRIV LOG_AUTH
#endif


/* LX06 unified glibc 2.25 stat bridge:
   gnu99 下 _LARGEFILE_SOURCE 默认开，glibc 头把 stat/lstat/fstat 重定向到
   64 位接口（__xstat64 等）。2.25 的 __xstat64(vers=3) 按旧 kernel stat64
   布局（104B, st_size@40）写入，与 -D_TIME_BITS=32 新头文件的 88B struct
   stat（st_size@28）错配 → st_size 读错 + 越界写踩栈 canary →
   __stack_chk_fail 启动即崩（V22 真机 -b banner 崩溃根因，2026-10-11 gdb
   + qemu-arm -g 定位）。
   实测（qemu-arm + 设备 2.25 libc）：__xstat(3, p, st) 走 statx，精确写入
   88B 布局（/etc/banner st_size=328, st_mode=0100664 ✓）；__xstat64(3) 的
   st_size 为垃圾值。故统一桥到 32 位接口 vers=3。scp/loginrec/sshpty/
   svr-authpubkey 等 16 处 stat/fstat 调用点全部经此桥，一处修全树生效。 */
extern int __xstat(int __v, const char *__p, struct stat *__st);
extern int __lxstat(int __v, const char *__p, struct stat *__st);
extern int __fxstat(int __v, int __fd, struct stat *__st);
#undef stat
#undef lstat
#undef fstat
#define stat(__p, __st) __xstat(3, (__p), (__st))
#define lstat(__p, __st) __lxstat(3, (__p), (__st))
#define fstat(__fd, __st) __fxstat(3, (__fd), (__st))
/* so we can avoid warnings about unused params (ie in signal handlers etc) */
#ifdef UNUSED 
#elif defined(__GNUC__) 
# define UNUSED(x) UNUSED_ ## x __attribute__((unused)) 
#elif defined(__LCLINT__) 
# define UNUSED(x) /*@unused@*/ x 
#else 
# define UNUSED(x) x 
#endif

#endif /* DROPBEAR_INCLUDES_H_ */
