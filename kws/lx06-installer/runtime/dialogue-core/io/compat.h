/* Stock LX06 uses ARM glibc 2.25. Modern cross headers redirect stat calls to
 * later symbols; use its published version-3 large-stat ABI explicitly. */
#ifndef LX06_DC_IO_COMPAT_H
#define LX06_DC_IO_COMPAT_H
#include <sys/stat.h>
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __fxstat64(int,int,struct stat64 *);
extern int __fxstatat64(int,int,const char *,struct stat64 *,int);
typedef struct stat64 dc_io_stat;
static inline int dc_io_fstat(int fd,dc_io_stat *s){return __fxstat64(3,fd,s);}
static inline int dc_io_fstatat(int fd,const char *p,dc_io_stat *s,int flags){return __fxstatat64(3,fd,p,s,flags);}
#else
typedef struct stat dc_io_stat;
static inline int dc_io_fstat(int fd,dc_io_stat *s){return fstat(fd,s);}
static inline int dc_io_fstatat(int fd,const char *p,dc_io_stat *s,int flags){return fstatat(fd,p,s,flags);}
#endif
#endif
