#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "system-clock.h"
#include <string.h>
#include <time.h>

/* GNU time.h supplies the actual libc timex layout and STA_* definitions.
 * The fixed OEM headers do not ship the optional sys/timex.h wrapper. Declare
 * its existing query ABI without copying a layout or falling back to host
 * headers. Linux's synchronized adjtimex return state TIME_OK is zero. */
extern int adjtimex(struct timex *);
enum { LX06_TIMEX_TIME_OK = 0 };

#define NS_PER_S UINT64_C(1000000000)
#define NS_PER_MS UINT64_C(1000000)
/* Linux timex.tolerance is scaled ppm (16 fractional bits). The kernel
 * NTP MAXFREQ is 500000 ns/s = 500 ppm, not an application clock grace. */
#define PPM_SCALE UINT64_C(65536000000)
#define MAX_TOLERANCE (500UL*65536UL)
/* Reject an interrupted/slow acquisition instead of returning an old wall
 * value as a current event-loop sample. This is only a freshness bound. */
#define MAX_SAMPLE_NS UINT64_C(100000000)
#define MAX_ERROR_US 16000000L

static int timestamp(const struct timespec *t,uint64_t *ns) {
    if(t->tv_sec<0||t->tv_nsec<0||t->tv_nsec>=1000000000L||
       (uint64_t)t->tv_sec>((uint64_t)INT64_MAX-(uint64_t)t->tv_nsec)/NS_PER_S)return 0;
    *ns=(uint64_t)t->tv_sec*NS_PER_S+(uint64_t)t->tv_nsec;return 1;
}
static int synchronized(int state,const struct timex *t) {
    const unsigned known=STA_PLL|STA_PPSFREQ|STA_PPSTIME|STA_FLL|STA_INS|STA_DEL|
        STA_UNSYNC|STA_FREQHOLD|STA_PPSSIGNAL|STA_PPSJITTER|STA_PPSWANDER|STA_PPSERROR|
        STA_CLOCKERR|STA_NANO|STA_MODE|STA_CLK;
    if(state!=LX06_TIMEX_TIME_OK||((unsigned)t->status&~known)||
       (t->status&(STA_UNSYNC|STA_CLOCKERR|STA_INS|STA_DEL|STA_PPSJITTER|STA_PPSWANDER|STA_PPSERROR))||
       t->tolerance<=0||(unsigned long)t->tolerance>MAX_TOLERANCE||
       t->freq>t->tolerance||t->freq< -t->tolerance||
       t->maxerror<0||t->maxerror>=MAX_ERROR_US||t->esterror<0||t->esterror>=MAX_ERROR_US)return 0;
    if((t->status&(STA_PPSFREQ|STA_PPSTIME))&&!(t->status&STA_PPSSIGNAL))return 0;
    return 1;
}
static uint64_t drift_bound(uint64_t elapsed,unsigned long tolerance) {
    /* Decomposition avoids wide integer extensions on the original ARM ABI.
     * The remainder product is <=65535999999*32768000 < INT64_MAX. */
    uint64_t whole=(elapsed/PPM_SCALE)*(uint64_t)tolerance;
    uint64_t rest=(elapsed%PPM_SCALE)*(uint64_t)tolerance;
    return whole+rest/PPM_SCALE+(rest%PPM_SCALE!=0);
}
int lx06_system_clock_read(struct lx06_system_clock *s,int64_t *wall_ms,uint64_t *mono_ms) {
    if(!s||!wall_ms||!mono_ms)return -1;
    *wall_ms=0;*mono_ms=s->have_mono?s->mono_ns/NS_PER_MS:0;
    struct timespec before={0},wall={0},after={0};struct timex first,last;
    memset(&first,0,sizeof(first));memset(&last,0,sizeof(last));
    /* modes stays zero: these calls query discipline and never adjust time. */
    int a=clock_gettime(CLOCK_MONOTONIC,&before);
    int b=adjtimex(&first);
    int c=clock_gettime(CLOCK_REALTIME,&wall);
    int d=adjtimex(&last);
    int e=clock_gettime(CLOCK_MONOTONIC,&after);
    uint64_t lo=0,wall_ns=0,hi=0;
    if(a||c||e||!timestamp(&before,&lo)||!timestamp(&wall,&wall_ns)||
       !timestamp(&after,&hi)||!hi||hi<=lo||hi-lo>MAX_SAMPLE_NS)goto untrusted;
    *wall_ms=(int64_t)(wall_ns/NS_PER_MS);*mono_ms=hi/NS_PER_MS;
    if(!*mono_ms)goto untrusted;
    uint64_t previous=s->mono_ns;
    if(s->have_mono&&hi<=previous)goto untrusted;
    s->mono_ns=hi;s->have_mono=1;
    if(!synchronized(b,&first)||!synchronized(d,&last))goto untrusted;
    unsigned long tolerance=(unsigned long)first.tolerance;
    if((unsigned long)last.tolerance>tolerance)tolerance=(unsigned long)last.tolerance;
    int64_t low=(int64_t)wall_ns-(int64_t)hi,high=(int64_t)wall_ns-(int64_t)lo;
    if(s->baseline) {
        unsigned long bound=tolerance>s->tolerance_scaled?tolerance:s->tolerance_scaled;
        uint64_t allowed=drift_bound(hi-previous,bound);
        if((low>s->offset_high_ns&&(uint64_t)low-(uint64_t)s->offset_high_ns>allowed)||
           (high<s->offset_low_ns&&(uint64_t)s->offset_low_ns-(uint64_t)high>allowed))goto untrusted;
        s->offset_low_ns=low;s->offset_high_ns=high;s->tolerance_scaled=tolerance;return 1;
    }
    s->offset_low_ns=low;s->offset_high_ns=high;s->tolerance_scaled=tolerance;s->baseline=1;return 0;
untrusted:
    s->baseline=0;return 0;
}
