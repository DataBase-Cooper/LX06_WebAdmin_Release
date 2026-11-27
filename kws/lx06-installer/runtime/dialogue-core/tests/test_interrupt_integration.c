/* Actual session decisions, reactor callbacks, route and player worker; synthetic PCM only. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <stdatomic.h>
#ifdef NDEBUG
#error assertions required
#endif
#include <assert.h>
static int asr_polled;
int lx06_control_asr_poll(lx06_control_asr *a,char *text,size_t cap,uint64_t now){(void)a;(void)now;const char input[]="等一下\xff";assert(cap>=sizeof input);if(asr_polled++)return 0;memcpy(text,input,sizeof input);return 1;}
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *a){(void)a;return 1;}
uint64_t lx06_control_asr_result_serial(const lx06_control_asr *a){(void)a;return 2;}
const char *lx06_control_asr_partial(const lx06_control_asr *a,uint64_t *reply){(void)a;*reply=0;return "";}
uint64_t lx06_control_asr_partial_serial(const lx06_control_asr *a){(void)a;return 0;}
struct sink { atomic_int opened, gate, dropped; atomic_uint frames; };
static int sopen(void *p,unsigned r){(void)r;atomic_store(&((struct sink*)p)->opened,1);return 0;}
static int spoll(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}
static ssize_t swrite(void *p,const int16_t *s,size_t n){(void)s;atomic_fetch_add(&((struct sink*)p)->frames,(unsigned)n);return (ssize_t)n;}
static int sdelay(void *p,long *n){(void)p;*n=100;return 0;}
static int sprepare(void *p){(void)p;return 0;}
static int sdrop(void *p){struct sink *s=p;while(!atomic_load(&s->gate))usleep(1000);atomic_store(&s->dropped,1);return 0;}
static void sclose(void *p){(void)p;}
struct observed_core {struct core core;unsigned stops,cancels,closes;};
static void observed_stop(void *p,dc_tag tag){struct observed_core *o=p;o->stops++;stop_local(&o->core,tag);}
static void observed_cancel(void *p,dc_tag tag){struct observed_core *o=p;assert(o->stops==1&&!o->core.player_live&&o->core.stopping_player);o->cancels++;cancel_remote(&o->core,tag);}
static void observed_close(void *p){struct observed_core *o=p;assert(o->stops==1&&o->cancels==1);o->closes++;}
static void fixture_close(void *p){(void)p;}
static void fixture_light(void *p,dc_tag tag,uint64_t seq,enum dc_state state){(void)p;(void)tag;(void)seq;(void)state;}
static void wait_atomic(atomic_int *v){for(int i=0;i<5000&&!atomic_load(v);i++)usleep(1000);assert(atomic_load(v));}
int main(void){
 struct core c={0};struct sink s={0};dc_config cfg={.barge_any=1};
 dc_io_playback_backend b={&s,sopen,spoll,swrite,sdelay,sprepare,sdrop,sclose};
 dc_session_ops ops={&c,stop_local,cancel_remote,question,fixture_close,fixture_light};
 c.player=dc_io_player_create(&b);assert(c.player);c.audio=1;c.route=dc_audio_route_create(2,1);assert(c.route);
 assert(!dc_audio_route_ready(c.route));assert(!dc_session_open(&c.session,2,&cfg,&ops,NULL,100));
 dc_tag old=dc_session_greeting(&c.session,"hello",101), bound={0};assert(old.session_epoch==2);
 assert(!dc_audio_route_greeting(c.route,old,101));assert(dc_audio_route_begin(c.route,9,102,&bound)==DC_AUDIO_BEGIN_KNOWN);
 dc_audio_action a;assert(dc_audio_route_next(c.route,&a)==1&&a.type==DC_AUDIO_GRANT);assert(!dc_audio_route_sent(c.route,&a,103));
 int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)(i*7+1);unsigned char expected[sizeof pcm*2];memcpy(expected,pcm,sizeof pcm);memcpy(expected+sizeof pcm,pcm,sizeof pcm);
 assert(!begin_player(&c,old,16000));assert(!dc_io_player_write(c.player,old,pcm,sizeof pcm));wait_atomic(&s.opened);
 /* Old-session FINAL must not preempt the current greeting. */
 assert(dc_session_text(&c.session,(dc_tag){1,old.reply_generation,7},"stale question",1,104)==0);
 assert(dc_session_text(&c.session,(dc_tag){2,old.reply_generation+1,7},"stale question",1,104)==0);
 assert(dc_session_text(&c.session,(dc_tag){2,old.reply_generation,0},"invalid question",1,104)==-1);
 assert(c.session.control.reply_live&&c.player_live&&!c.stopping_player);
 /* Confirmed near-end voice causes actual STOP then queues remote cancel. */
 dc_tag source={2,old.reply_generation,8};assert(!dc_audio_route_capture(c.route,pcm,sizeof pcm,8,1,105));
 assert(dc_session_voice(&c.session,source,1,0,106)==1);assert(!c.player_live&&c.stopping_player&&!c.failed);
 dc_audio_status status;dc_audio_route_status(c.route,&status);assert(status.cancel_barrier&&status.microphone_bytes==sizeof pcm);
 assert(dc_audio_route_next(c.route,&a)==1&&a.type==DC_AUDIO_CANCEL);assert(!dc_audio_route_sent(c.route,&a,107));
 assert(dc_audio_route_next(c.route,&a)==0);assert(!atomic_load(&s.dropped));
 assert(dc_session_voice(&c.session,source,1,0,108)==0);
 assert(!dc_audio_route_capture(c.route,pcm,sizeof pcm,8,1,109));
 assert(dc_session_text(&c.session,source,"complete followup",1,110)>0);
 assert(!dc_audio_route_final(c.route,8));dc_audio_route_status(c.route,&status);
 assert(status.microphone_bytes==2*sizeof pcm&&status.cancel_barrier&&status.pending_tag.utterance_id==8);
 atomic_store(&s.gate,1);wait_atomic(&s.dropped);
 for(int i=0;i<5000&&c.stopping_player;i++){player_events(&c,111);usleep(1000);}assert(!c.stopping_player);
 dc_tag next=c.session.control.tag;assert(next.session_epoch==2&&next.reply_generation!=old.reply_generation);
 assert(!begin_player(&c,next,16000));stop_local(&c,old);assert(c.player_live&&!c.stopping_player);
 unsigned before=atomic_load(&s.frames);assert(!dc_io_player_write(c.player,next,pcm,sizeof pcm));
 for(int i=0;i<5000&&atomic_load(&s.frames)<before+320;i++)usleep(1000);
 assert(atomic_load(&s.frames)==before+320); /* next PCM played before remote ACK */
 assert(!dc_audio_route_resumed(c.route,9,old.reply_generation+1));assert(dc_audio_route_resumed(c.route,9,old.reply_generation)==1);
 size_t sent=0;while(dc_audio_route_next(c.route,&a)==1&&a.type==DC_AUDIO_MIC){assert(sent+a.bytes<=sizeof expected);assert(!memcmp(a.pcm,expected+sent,a.bytes));sent+=a.bytes;assert(!dc_audio_route_sent(c.route,&a,112));}
 assert(sent==2*sizeof pcm&&a.type==DC_AUDIO_COMMIT);
 stop_local(&c,next);for(int i=0;i<5000&&c.stopping_player;i++){player_events(&c,112);usleep(1000);}assert(!c.stopping_player);
 dc_io_player_destroy(c.player);dc_audio_route_destroy(c.route);
 /* Non-greeting live reply, actual recognition/reactor/session/control path. */
 struct core bad={0};struct sink sink2={0};atomic_store(&sink2.gate,1);
 dc_io_playback_backend b2={&sink2,sopen,spoll,swrite,sdelay,sprepare,sdrop,sclose};
 dc_config cfg2={0};strcpy(cfg2.interrupt,"等一下,等等");
 dc_session_ops ops2={&bad,stop_local,cancel_remote,question,fixture_close,fixture_light};
 bad.player=dc_io_player_create(&b2);assert(bad.player);bad.audio=1;bad.route=dc_audio_route_create(20,0);assert(bad.route);
 assert(!dc_audio_route_ready(bad.route));assert(!dc_session_open(&bad.session,20,&cfg2,&ops2,NULL,100));
 assert(!dc_session_ready(&bad.session,101));dc_tag active=dc_session_reply(&bad.session,1,102);assert(active.reply_generation==1);
 assert(!dc_audio_route_greeting(bad.route,active,102));assert(!begin_player(&bad,active,16000));
 assert(!dc_io_player_write(bad.player,active,pcm,sizeof pcm));wait_atomic(&sink2.opened);
 dc_control before_control=bad.session.control;bad.asr=(lx06_control_asr *)&bad;
 recognition(&bad,103);assert(asr_polled==1&&bad.failed);
 assert(bad.player_live&&!bad.stopping_player&&!atomic_load(&sink2.dropped));
 if(memcmp(&before_control,&bad.session.control,sizeof before_control)){fprintf(stderr,"FAIL: malformed final mutated control state\n");return 1;}
 assert(dc_session_text(&bad.session,(dc_tag){20,1,2},"等一下\xff",0,103)==-1);
 assert(!memcmp(&before_control,&bad.session.control,sizeof before_control));
 assert(bad.player_live&&!bad.stopping_player&&!atomic_load(&sink2.dropped));
 /* Recognition flags malformed input as fatal; exercise serial recovery at
  * session level without claiming the live reactor continues after failure. */
 dc_tag valid={20,active.reply_generation,2};
 assert(dc_session_text(&bad.session,valid,"等一下，完整问题",1,104)==1);
 assert(!bad.player_live&&bad.stopping_player&&bad.session.control.last_final_utterance==2);
 assert(bad.session.control.tag.reply_generation==2&&bad.session.control.tag.utterance_id==2);
 for(int i=0;i<5000&&bad.stopping_player;i++){player_events(&bad,105);usleep(1000);}assert(!bad.stopping_player);
 dc_io_player_destroy(bad.player);dc_audio_route_destroy(bad.route);
 /* Final goodbye during active greeting: production stop and cancellation
  * precede close without waiting for synthetic backend drop. */
 struct observed_core bye={0};struct sink sink3={0};
 dc_io_playback_backend b3={&sink3,sopen,spoll,swrite,sdelay,sprepare,sdrop,sclose};
 dc_config cfg3={0};strcpy(cfg3.interrupt,"等一下");strcpy(cfg3.goodbye,"结束对话");
 dc_session_ops ops3={&bye,observed_stop,observed_cancel,question,observed_close,fixture_light};
 bye.core.player=dc_io_player_create(&b3);assert(bye.core.player);bye.core.audio=1;
 bye.core.route=dc_audio_route_create(30,0);assert(bye.core.route);
 assert(!dc_audio_route_ready(bye.core.route));assert(!dc_session_open(&bye.core.session,30,&cfg3,&ops3,NULL,100));
 dc_tag greeting=dc_session_greeting(&bye.core.session,"hello",101);assert(greeting.session_epoch);
 assert(!dc_audio_route_greeting(bye.core.route,greeting,101));assert(!begin_player(&bye.core,greeting,16000));
 assert(!dc_io_player_write(bye.core.player,greeting,pcm,sizeof pcm));wait_atomic(&sink3.opened);
 for(int i=0;i<5000&&atomic_load(&sink3.frames)<320;i++)usleep(1000);
 assert(atomic_load(&sink3.frames)>=320);
 assert(dc_session_text(&bye.core.session,(dc_tag){30,greeting.reply_generation,2},"等一下，结束对话",1,102)==1);
 assert(bye.stops==1&&bye.cancels==1&&bye.closes==1&&!bye.core.failed&&!atomic_load(&sink3.dropped));
 assert(dc_session_text(&bye.core.session,(dc_tag){30,greeting.reply_generation,2},"结束对话",1,103)==0);
 assert(bye.stops==1&&bye.cancels==1&&bye.closes==1);
 atomic_store(&sink3.gate,1);wait_atomic(&sink3.dropped);
 for(int i=0;i<5000&&bye.core.stopping_player;i++){player_events(&bye.core,104);usleep(1000);}assert(!bye.core.stopping_player);
 dc_io_player_destroy(bye.core.player);dc_audio_route_destroy(bye.core.route);
 puts("integrated interrupt, greeting goodbye and malformed ASR: PASS (synthetic only)");return 0;
}
