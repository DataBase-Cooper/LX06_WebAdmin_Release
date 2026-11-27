/* Offline: actual core backend wrapper, voice guard and player stop worker.
 * Synthetic clock and sink only; no capture producer or acoustic acceptance. */
#define _GNU_SOURCE
#include <time.h>
#include <stdatomic.h>
static atomic_ulong fake_ms;
static int fixture_clock(clockid_t id,struct timespec *ts) {
    (void)id; unsigned long n=atomic_load(&fake_ms);
    ts->tv_sec=(time_t)(n/1000);ts->tv_nsec=(long)(n%1000)*1000000;return 0;
}
#define clock_gettime fixture_clock
#define main dc_unused_program_main
#include "../main.c"
#undef main
#undef clock_gettime
#include <math.h>
#ifdef NDEBUG
#error assertions required
#endif
#include <assert.h>
struct sink {atomic_uint writes,drops; int result_override; ssize_t result;};
static int s_open(void *p,unsigned r){(void)p;(void)r;return 0;}
static int s_poll(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}
static ssize_t s_write(void *p,const int16_t *s,size_t n){(void)s;struct sink *x=p;atomic_fetch_add(&x->writes,1);return x->result_override?x->result:(ssize_t)n;}
static int s_delay(void *p,long *n){(void)p;*n=100;return 0;}
static int s_prepare(void *p){(void)p;return 0;}
static int s_drop(void *p){struct sink *x=p;atomic_fetch_add(&x->drops,1);return 0;}
static void s_close(void *p){(void)p;}
static dc_voice_result check(dc_voice_guard *g,const int16_t *pcm,uint64_t t){int16_t out[480];dc_voice_result r;assert(!dc_voice_guard_process(g,pcm,480,0,out,t,&r));return r;}
static dc_voice_result check10(dc_voice_guard *g,const int16_t *pcm,uint64_t t){int16_t out[160];dc_voice_result r;assert(!dc_voice_guard_process(g,pcm,160,0,out,t,&r));return r;}
/* Isolated guard timeline cases explicitly invoke the production callback;
 * worker admission and ordering are tested separately below and in fault test. */
static ssize_t fixture_write(struct core *c,const int16_t *pcm,size_t n) {
 ssize_t written=backend_write(c,pcm,n);
 static uint64_t operation;
 if(written>0&&(size_t)written<=n)player_reference_accepted(c,(dc_tag){1,1,1},pcm,(size_t)written,c->backend_rate,++operation,0);
 return written;
}
int main(void) {
 struct sink s={0};struct core c={0};c.guard=dc_voice_guard_create();assert(c.guard);
 c.real_backend=(dc_io_playback_backend){&s,s_open,s_poll,s_write,s_delay,s_prepare,s_drop,s_close};
 assert(!backend_open(&c,16000));
 int16_t wave[480],zero[160]={0};for(unsigned i=0;i<480;i++)wave[i]=(int16_t)(1200*sin(i*.147)+300*cos(i*.087));
 /* Without a writer continuity marker, even same-ms writes may straddle an
  * unobserved gap. Do not construct a reference window across callbacks. */
 atomic_store(&fake_ms,400);
 assert(fixture_write(&c,wave,1)==1);
 assert(fixture_write(&c,wave+1,159)==159);
 assert(!check10(c.guard,wave,400).echo); /* regression remains fail-closed */
 assert(fixture_write(&c,wave+160,320)==320);
 dc_voice_result fragmented=check(c.guard,wave,400);
 if(fragmented.echo){fprintf(stderr,"unproven fragmented window joined: %f\n",fragmented.correlation);return 1;}
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 /* Error between partial accepts, with an unchanged coarse timestamp. */
 s.result_override=1;s.result=80;
 assert(fixture_write(&c,wave,160)==80);
 s.result=-EPIPE;assert(fixture_write(&c,wave+80,80)==-EPIPE);
 s.result=80;assert(fixture_write(&c,wave+80,80)==80);
 assert(!check10(c.guard,wave,400).echo);
 s.result_override=0;
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 /* Only accepted frames become reference: no publication on EAGAIN or zero,
  * and a partial return must not publish the unaccepted suffix. */
 atomic_store(&fake_ms,500);s.result_override=1;s.result=-EAGAIN;
 assert(fixture_write(&c,wave,480)==-EAGAIN);assert(!check(c.guard,wave,500).echo);
 s.result=0;assert(fixture_write(&c,wave,480)==0);assert(!check(c.guard,wave,500).echo);
 s.result=160;assert(fixture_write(&c,wave,480)==160);assert(!check(c.guard,wave,500).echo);
 assert(fixture_write(&c,wave+160,320)==160);assert(!check(c.guard,wave,500).echo);
 assert(fixture_write(&c,wave+320,160)==160);assert(!check(c.guard,wave,500).echo);
 s.result_override=0;dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 atomic_store(&fake_ms,1000);assert(fixture_write(&c,wave,480)==480);
 assert(check(c.guard,wave,1000).echo);
 assert(!check(c.guard,wave,999).echo);
 assert(!check(c.guard,wave,1700).echo);
 /* A fresh short silent write must not make 800ms-old PCM current again. */
 atomic_store(&fake_ms,1800);assert(fixture_write(&c,zero,160)==160);
 dc_voice_result r=check(c.guard,wave,1800);
 if(r.echo||r.correlation!=0){fprintf(stderr,"stale reference revived: correlation=%f\n",r.correlation);return 1;}
 /* Several sub-700ms writes must not renew an older matching waveform. */
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 atomic_store(&fake_ms,1000);assert(fixture_write(&c,wave,480)==480);
 atomic_store(&fake_ms,1600);assert(fixture_write(&c,zero,160)==160);
 atomic_store(&fake_ms,1800);assert(fixture_write(&c,zero,160)==160);
 r=check(c.guard,wave,1800);
 if(r.echo||r.correlation!=0){fprintf(stderr,"short-gap stale reference revived: correlation=%f\n",r.correlation);return 1;}
 /* Separated partial writes are not one contiguous reference window. */
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 s.result_override=1;s.result=160;
 atomic_store(&fake_ms,5000);assert(fixture_write(&c,wave,480)==160);
 atomic_store(&fake_ms,5100);assert(fixture_write(&c,wave+160,320)==160);
 atomic_store(&fake_ms,5200);assert(fixture_write(&c,wave+320,160)==160);
 r=check(c.guard,wave,5200);
 if(r.echo){fprintf(stderr,"discontinuous partial writes joined: correlation=%f\n",r.correlation);return 1;}
 s.result_override=0;
 /* Expiring even one partial write invalidates a spanning window. */
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 s.result_override=1;s.result=160;
 atomic_store(&fake_ms,1000);assert(fixture_write(&c,wave,480)==160);
 atomic_store(&fake_ms,1010);assert(fixture_write(&c,wave+160,320)==160);
 atomic_store(&fake_ms,1020);assert(fixture_write(&c,wave+320,160)==160);
 assert(!check(c.guard,wave,1699).echo);
 r=check(c.guard,wave,1700);assert(!r.echo&&r.correlation==0);
 s.result_override=0;
 /* A future newer write must not hide independently valid older samples. */
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 atomic_store(&fake_ms,1000);assert(fixture_write(&c,wave,480)==480);
 atomic_store(&fake_ms,1100);assert(fixture_write(&c,zero,160)==160);
 assert(check(c.guard,wave,1000).echo);
 assert(!check(c.guard,wave,999).echo);
 /* Wrap the ring, retain a matching old window, then cross its exact expiry. */
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 int16_t silent[480]={0};atomic_store(&fake_ms,1000);
 for(unsigned i=0;i<33;i++)assert(fixture_write(&c,silent,480)==480);
 assert(fixture_write(&c,wave,480)==480);
 atomic_store(&fake_ms,1600);assert(fixture_write(&c,zero,160)==160);
 assert(check(c.guard,wave,1699).echo);
 r=check(c.guard,wave,1700);assert(!r.echo&&r.correlation==0);
 /* A backwards timestamp must not reactivate samples from the future. */
 atomic_store(&fake_ms,3000);assert(fixture_write(&c,wave,480)==480);
 atomic_store(&fake_ms,2900);assert(fixture_write(&c,zero,160)==160);
 r=check(c.guard,wave,2900);assert(!r.echo&&r.correlation==0);
 dc_voice_guard_destroy(c.guard);c.guard=dc_voice_guard_create();assert(c.guard);
 dc_io_playback_backend wrapped={&c,backend_open,backend_poll,backend_write,backend_delay,backend_prepare,backend_drop,backend_close};
 dc_io_reference reference={&c,player_reference_reset,player_reference_accepted};
 c.player=dc_io_player_create_with_reference(&wrapped,&reference);assert(c.player);dc_tag tag={1,1,1};
 atomic_store(&fake_ms,1800);unsigned before=atomic_load(&s.writes);
 assert(!begin_player(&c,tag,16000));assert(!dc_io_player_write(c.player,tag,wave,sizeof wave));
 for(int i=0;i<3000&&atomic_load(&s.writes)<before+3;i++)usleep(1000);
 assert(atomic_load(&s.writes)>=before+3);
 /* The worker writes <=10ms; a 30ms window cannot cross callbacks. */
 r=check(c.guard,wave,1800);assert(!r.echo);
 stop_local(&c,tag);
 for(int i=0;i<3000&&c.stopping_player;i++){player_events(&c,1800);usleep(1000);}
 assert(!c.stopping_player&&atomic_load(&s.drops)==1);
 assert(!check(c.guard,wave,1800).echo); /* no cross-write tail window */
 assert(!check(c.guard,wave,2500).echo);
 dc_io_player_destroy(c.player);dc_voice_guard_destroy(c.guard);
 puts("reference timeline and real player abort: PASS (synthetic only)");return 0;
}
