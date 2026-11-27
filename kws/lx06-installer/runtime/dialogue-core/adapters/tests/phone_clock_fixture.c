#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Offline fixture only, enabled by an explicit build switch. Production
 * binaries never read this environment or wrap the system clock. */
extern int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *out){
    static unsigned wall_calls;
    const char *mode=getenv("PHONE_FIXTURE_CLOCK");
    if(mode&&clock==CLOCK_MONOTONIC&&!strcmp(mode,"mono_unavailable")){errno=EIO;return -1;}
    if(clock!=CLOCK_REALTIME||!mode)return __real_clock_gettime(clock,out);
    if(!strcmp(mode,"unavailable")){errno=EIO;return -1;}
    if(!strcmp(mode,"zero")){out->tv_sec=0;out->tv_nsec=0;return 0;}
    if(!strcmp(mode,"future")){out->tv_sec=(time_t)4102444800ULL;out->tv_nsec=0;return 0;}
    int result=__real_clock_gettime(clock,out);
    if(!result&&wall_calls++&&!strcmp(mode,"backwards_after_first")){out->tv_sec=0;out->tv_nsec=0;}
    return result;
}
