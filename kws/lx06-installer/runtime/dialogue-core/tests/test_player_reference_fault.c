/* Actual player worker -> production reference callbacks -> real voice guard.
 * Only the ALSA backend and clock are synthetic; no device or network. */
#define _GNU_SOURCE
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
static atomic_ulong fake_ms;
static int fixture_clock(clockid_t id,struct timespec *ts) {
 (void)id;unsigned long n=atomic_load(&fake_ms);
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
static int s_open(void *p,unsigned r){(void)p;(void)r;return 0;}
static int s_poll(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}

static int s_prepare(void *p){(void)p;return 0;}
static void s_close(void *p){(void)p;}
static dc_voice_result check10(dc_voice_guard *g,const int16_t *pcm,uint64_t t){int16_t out[160];dc_voice_result r;assert(!dc_voice_guard_process(g,pcm,160,0,out,t,&r));return r;}

struct fault_sink {
 pthread_mutex_t lock;
 pthread_cond_t cond;
 int gate, entered, released, fragment, callback_gate, callback_entered, callback_released;
 unsigned calls, completed, published;
 const int16_t *ptr[16];
 size_t requested[16];
 ssize_t accepted[16],script[16];unsigned script_n;
 uint64_t operation[16],offset[16];
 atomic_int drain;
};
static int fault_delay(void *ctx,long *n){struct fault_sink *s=ctx;*n=atomic_load(&s->drain)?0:100;return 0;}
static int fault_drop(void *ctx) {(void)ctx;return 0;}
static ssize_t fault_write(void *ctx,const int16_t *pcm,size_t frames) {
 struct fault_sink *s=ctx;
 pthread_mutex_lock(&s->lock);
 unsigned i=s->calls++; assert(i<16);
 s->ptr[i]=pcm;s->requested[i]=frames;
 if(s->gate){s->entered=1;pthread_cond_broadcast(&s->cond);while(!s->released)pthread_cond_wait(&s->cond,&s->lock);}
 ssize_t n=i<s->script_n?s->script[i]:(s->fragment && i==0 ? 1 : (ssize_t)frames);
 s->accepted[i]=n;
 pthread_mutex_unlock(&s->lock);
 return n;
}
/* Observe actual player calls at the production wrapper boundary, without
 * replacing the wrapper or introducing a fabricated operation identifier. */
static ssize_t observed_write(void *ctx,const int16_t *pcm,size_t frames) {
 struct core *c=ctx;struct fault_sink *s=c->real_backend.context;
 ssize_t n=backend_write(ctx,pcm,frames);
 pthread_mutex_lock(&s->lock);s->completed++;pthread_cond_broadcast(&s->cond);pthread_mutex_unlock(&s->lock);
 return n;
}
static void observed_reference(void *ctx,dc_tag tag,const int16_t *pcm,size_t frames,unsigned rate,uint64_t operation,uint64_t offset) {
 struct core *c=ctx;struct fault_sink *s=c->real_backend.context;
 player_reference_accepted(ctx,tag,pcm,frames,rate,operation,offset);
 pthread_mutex_lock(&s->lock);assert(s->published<16);s->operation[s->published]=operation;s->offset[s->published]=offset;s->published++;s->callback_entered=1;pthread_cond_broadcast(&s->cond);
 while(s->callback_gate&&!s->callback_released)pthread_cond_wait(&s->cond,&s->lock);
 pthread_mutex_unlock(&s->lock);
}
struct stop_job {dc_io_player *player;dc_tag tag;atomic_int started,done;int result;};
static void *run_stop(void *arg) {struct stop_job *j=arg;atomic_store(&j->started,1);j->result=dc_io_player_stop(j->player,j->tag);atomic_store(&j->done,1);return NULL;}
static void wait_published(struct fault_sink *s,unsigned n) {
 for(int i=0;i<5000;i++) {pthread_mutex_lock(&s->lock);int ok=s->published>=n;pthread_mutex_unlock(&s->lock);if(ok)return;usleep(1000);}
 fprintf(stderr,"timeout waiting for publication\n");abort();
}
static void wait_count(struct fault_sink *s,unsigned n,int entered) {
 for(int i=0;i<5000;i++) {
  pthread_mutex_lock(&s->lock);int ok=entered?s->entered:(s->completed>=n);pthread_mutex_unlock(&s->lock);
  if(ok)return;
  usleep(1000);
 }
 fprintf(stderr,"timeout waiting for worker\n");abort();
}
static void begin(struct core *c,dc_tag tag) {int rc=dc_io_player_begin(c->player,tag,16000);assert(rc==0);}
static void stop_and_wait(struct core *c,dc_tag tag) {
 int rc=dc_io_player_stop(c->player,tag);assert(rc==0);
 for(int i=0;i<5000;i++) {
  dc_io_event e;while(dc_io_player_next_event(c->player,&e)>0)
   if(e.type==DC_IO_STOPPED && dc_tag_equal(e.tag,tag))return;
  usleep(1000);
 }
 fprintf(stderr,"timeout waiting for stop\n");abort();
}
static void init(struct core *c,struct fault_sink *s) {
 memset(c,0,sizeof *c);memset(s,0,sizeof *s);
 assert(!pthread_mutex_init(&s->lock,NULL));assert(!pthread_cond_init(&s->cond,NULL));
 c->guard=dc_voice_guard_create();assert(c->guard);
 atomic_init(&s->drain,0);
 c->real_backend=(dc_io_playback_backend){s,s_open,s_poll,fault_write,fault_delay,s_prepare,fault_drop,s_close};
 dc_io_playback_backend b={c,backend_open,backend_poll,observed_write,backend_delay,backend_prepare,backend_drop,backend_close};
 dc_io_reference ref={c,player_reference_reset,observed_reference};
 c->player=dc_io_player_create_with_reference(&b,&ref);assert(c->player);
}
static void cleanup(struct core *c,struct fault_sink *s) {
 dc_io_player_destroy(c->player);dc_voice_guard_destroy(c->guard);
 pthread_cond_destroy(&s->cond);pthread_mutex_destroy(&s->lock);
}
static void session_stop(void *v,dc_tag t){stop_local(v,t);}
static void session_cancel(void *v,dc_tag t){(void)v;(void)t;}
static int session_question(void *v,dc_tag t,const char *q){(void)v;(void)t;(void)q;return 0;}
static void session_close(void *v){(void)v;}
static void session_light(void *v,dc_tag t,uint64_t s,enum dc_state state){(void)v;(void)t;(void)s;(void)state;}
int main(void) {
 atomic_store(&fake_ms,1000);
 int16_t wave[160];for(unsigned i=0;i<160;i++)wave[i]=(int16_t)(1200*sin(i*.147)+300*cos(i*.087));
 dc_tag old={1,1,1},next={2,1,1};struct core c;struct fault_sink s;
 /* The 160-frame chunk really travels through the worker and wrapper twice. */
 init(&c,&s);s.fragment=1;begin(&c,old);
 int rc=dc_io_player_write(c.player,old,wave,sizeof wave);assert(!rc);wait_published(&s,2);
 assert(s.calls==2 && s.requested[0]==160 && s.accepted[0]==1 && s.requested[1]==159 && s.accepted[1]==159);
 assert(s.ptr[1]==s.ptr[0]+1);
 assert(s.operation[0]&&s.operation[0]==s.operation[1]&&s.offset[0]==0&&s.offset[1]==1);
 int defects=0;
 dc_voice_result r=check10(c.guard,wave,1000);
 printf("worker 1+159: ptr offset=1, requests=160/159, accepted=1/159, echo=%d corr=%.3f\n",r.echo,r.correlation);
 assert(r.echo); /* Accepted fragments of this worker operation must join. */
 stop_and_wait(&c,old);cleanup(&c,&s);
 /* Two sequential worker chunks must never join despite the same clock. */
 init(&c,&s);begin(&c,old);
 assert(!dc_io_player_write(c.player,old,wave,80*sizeof *wave));wait_published(&s,1);
 assert(!dc_io_player_write(c.player,old,wave+80,80*sizeof *wave));wait_published(&s,2);
 assert(s.operation[0]&&s.operation[1]&&s.operation[0]!=s.operation[1]);
 assert(s.offset[0]==0&&s.offset[1]==0&&!check10(c.guard,wave,1000).echo);
 stop_and_wait(&c,old);cleanup(&c,&s);
 /* Non-accepting retries preserve the offset, but recovery fences it. */
 init(&c,&s);s.script_n=5;s.script[0]=1;s.script[1]=-EAGAIN;s.script[2]=-EINTR;s.script[3]=0;s.script[4]=159;
 begin(&c,old);assert(!dc_io_player_write(c.player,old,wave,sizeof wave));wait_published(&s,2);
 assert(s.calls==5&&s.operation[0]==s.operation[1]&&s.offset[0]==0&&s.offset[1]==1);
 assert(check10(c.guard,wave,1000).echo);stop_and_wait(&c,old);cleanup(&c,&s);
 for(int k=0;k<2;k++) {
  init(&c,&s);s.script_n=3;s.script[0]=80;s.script[1]=k?-ESTRPIPE:-EPIPE;s.script[2]=80;
  begin(&c,old);assert(!dc_io_player_write(c.player,old,wave,sizeof wave));wait_published(&s,2);
  assert(s.operation[0]&&s.operation[1]&&s.operation[0]!=s.operation[1]);
  assert(s.offset[0]==0&&s.offset[1]==80&&!check10(c.guard,wave,1000).echo);
  stop_and_wait(&c,old);cleanup(&c,&s);
 }
 /* Hold the actual reference callback after publication while a stop races.
  * Stop must serialize after publication and clear it before returning. */
 init(&c,&s);s.callback_gate=1;begin(&c,old);rc=dc_io_player_write(c.player,old,wave,sizeof wave);assert(!rc);wait_published(&s,1);
 assert(check10(c.guard,wave,1000).echo);
 struct stop_job job={.player=c.player,.tag=old};pthread_t stopper;
 assert(!pthread_create(&stopper,NULL,run_stop,&job));
 while(!atomic_load(&job.started))usleep(1000);
 usleep(20000);assert(!atomic_load(&job.done));
 pthread_mutex_lock(&s.lock);s.callback_released=1;pthread_cond_broadcast(&s.cond);pthread_mutex_unlock(&s.lock);
 assert(!pthread_join(stopper,NULL));assert(!job.result);assert(!check10(c.guard,wave,1000).echo);
 stop_and_wait(&c,old);cleanup(&c,&s);
 /* A drained owner has no stop reset; a genuine new owner must clear it. */
 init(&c,&s);begin(&c,old);rc=dc_io_player_write(c.player,old,wave,sizeof wave);assert(!rc);wait_published(&s,1);
 assert(check10(c.guard,wave,1000).echo);atomic_store(&s.drain,1);assert(!dc_io_player_finish(c.player,old));
 for(int i=0;i<5000;i++){dc_io_event e;if(dc_io_player_next_event(c.player,&e)>0&&e.type==DC_IO_DRAINED)break;if(i==4999)abort();usleep(1000);}
 assert(check10(c.guard,wave,1000).echo);begin(&c,next);assert(!check10(c.guard,wave,1000).echo);
 stop_and_wait(&c,next);cleanup(&c,&s);
 /* Publication before stop must be cleared; a stale stop must not clear a new owner. */
 init(&c,&s);begin(&c,old);rc=dc_io_player_write(c.player,old,wave,sizeof wave);assert(!rc);wait_published(&s,1);
 r=check10(c.guard,wave,1000);assert(r.echo);
 stop_and_wait(&c,old);r=check10(c.guard,wave,1000);assert(!r.echo);begin(&c,next);r=check10(c.guard,wave,1000);
 printf("after stop/new session: old reference echo=%d corr=%.3f\n",r.echo,r.correlation);defects+=r.echo;
 rc=dc_io_player_write(c.player,next,wave,sizeof wave);assert(!rc);wait_published(&s,2);
 assert(check10(c.guard,wave,1000).echo);assert(dc_io_player_stop(c.player,old)==-ESTALE);
 assert(check10(c.guard,wave,1000).echo); /* stale stop cannot clear new owner */
 assert(!dc_io_player_begin(c.player,next,16000));assert(check10(c.guard,wave,1000).echo);
 stop_and_wait(&c,next);cleanup(&c,&s);
 /* Stop while backend write is blocked; release after revision change. */
 init(&c,&s);s.gate=1;begin(&c,old);rc=dc_io_player_write(c.player,old,wave,sizeof wave);assert(!rc);wait_count(&s,0,1);
 rc=dc_io_player_stop(c.player,old);assert(!rc);
 pthread_mutex_lock(&s.lock);s.released=1;pthread_cond_broadcast(&s.cond);pthread_mutex_unlock(&s.lock);
 wait_count(&s,1,0);
 for(int i=0;i<5000;i++){dc_io_event e;if(dc_io_player_next_event(c.player,&e)>0&&e.type==DC_IO_STOPPED)break;if(i==4999)abort();usleep(1000);}
 r=check10(c.guard,wave,1000);assert(!r.echo);assert(s.published==0);
 begin(&c,next);r=check10(c.guard,wave,1000);
 printf("late old write after stop/new session: echo=%d corr=%.3f\n",r.echo,r.correlation);defects+=r.echo;
 stop_and_wait(&c,next);cleanup(&c,&s);
 /* Actual session interruption -> reactor stop -> worker and guard. */
 init(&c,&s);dc_config config={0};strcpy(config.interrupt,"等一下");
 dc_session_ops ops={&c,session_stop,session_cancel,session_question,session_close,session_light};
 assert(!dc_session_open(&c.session,31,&config,&ops,"",100));assert(!dc_session_ready(&c.session,101));
 dc_tag session_old=dc_session_reply(&c.session,1,102);s.gate=1;
 assert(!begin_player(&c,session_old,16000));assert(!dc_io_player_write(c.player,session_old,wave,sizeof wave));wait_count(&s,0,1);
 assert(dc_session_text(&c.session,(dc_tag){31,session_old.reply_generation,2},"等一下",0,103)==1);
 assert(dc_session_text(&c.session,(dc_tag){31,session_old.reply_generation,2},"等一下，完整问题",1,104)==1);
 assert(c.stopping_player&&!c.player_live);pthread_mutex_lock(&s.lock);s.released=1;pthread_cond_broadcast(&s.cond);pthread_mutex_unlock(&s.lock);
 for(int i=0;i<5000&&c.stopping_player;i++){player_events(&c,1000);usleep(1000);}assert(!c.stopping_player&&s.published==0);
 assert(!check10(c.guard,wave,1000).echo);dc_tag session_next=c.session.control.tag;
 assert(!begin_player(&c,session_next,16000));assert(!dc_io_player_write(c.player,session_next,wave,sizeof wave));wait_published(&s,1);
 assert(check10(c.guard,wave,1000).echo);stop_local(&c,session_old);assert(check10(c.guard,wave,1000).echo);
 stop_local(&c,session_next);assert(!check10(c.guard,wave,1000).echo);cleanup(&c,&s);
 if(defects){fprintf(stderr,"RED: %d stop reference requirements unmet\n",defects);return 1;}
 puts("stop/reference ordering and fragmented 1+159: PASS (synthetic only)");return 0;
}
