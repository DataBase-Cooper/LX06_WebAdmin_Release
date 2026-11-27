/* Production capture/reactor/session/route, synthetic capture, ASR and guard boundaries. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static int available;
int dc_io_capture_next(dc_io_capture *c,dc_io_capture_frame *f){(void)c;if(!available)return 0;available=0;memset(f,0,sizeof *f);f->rate=16000;f->bytes=320;f->mode=DC_IO_CAPTURE_LXA1;memset(f->pcm,0x53,320);return 1;}
int dc_io_capture_fd(const dc_io_capture *c){(void)c;return -1;}
int dc_io_capture_stalled(const dc_io_capture *c,uint64_t n){(void)c;(void)n;return 0;}
int dc_voice_guard_process(dc_voice_guard *g,const int16_t *in,size_t n,double gain,int16_t *out,uint64_t now,dc_voice_result *v){(void)g;(void)gain;(void)now;memcpy(out,in,n*2);memset(v,0,sizeof *v);return 0;}
void lx06_control_asr_reply(lx06_control_asr *a,uint64_t g){(void)a;(void)g;}
void lx06_control_asr_mic_pending(lx06_control_asr *a,size_t n){(void)a;(void)n;}
int lx06_control_asr_feed_stamped(lx06_control_asr *a,int p,const void *pcm,size_t n,const struct qev_stamp *s,uint64_t now){(void)a;(void)p;(void)pcm;(void)n;(void)s;(void)now;return 0;}
void lx06_control_asr_bind_source(lx06_control_asr *a){(void)a;}
uint64_t lx06_control_asr_active_serial(const lx06_control_asr *a){(void)a;return 2;}
const char *lx06_control_asr_partial(const lx06_control_asr *a,uint64_t *g){(void)a;*g=0;return "";}
static void noop_stop(void *p,dc_tag t){(void)p;(void)t;}
static void noop_cancel(void *p,dc_tag t){(void)p;(void)t;}
static int noop_question(void *p,dc_tag t,const char *s){(void)p;(void)t;(void)s;return 0;}
static void closed(void *p){(*(int *)p)++;}
static void noop_light(void *p,dc_tag t,uint64_t s,enum dc_state st){(void)p;(void)t;(void)s;(void)st;}
static void feed(struct core *c,uint64_t now){available=1;capture(c,now);assert(!available);}
static void bounded_followup(void){
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,fd));
 struct core c={0};int close_count=0;dc_config cfg={0};
 dc_session_ops ops={&close_count,noop_stop,noop_cancel,noop_question,closed,noop_light};
 assert(!dc_session_open(&c.session,21,&cfg,&ops,"",100));assert(!dc_session_ready(&c.session,101));
 c.audio=c.ready=1;c.adapter_fd=fd[0];c.route=dc_audio_route_create(21,0);assert(c.route);assert(!dc_audio_route_ready(c.route));
 dc_tag old=dc_session_reply(&c.session,1,102),bound;assert(dc_audio_route_begin(c.route,8,103,&bound)==DC_AUDIO_BEGIN_NEED_TAG);assert(!dc_audio_route_bind(c.route,8,old,103));
 send_microphone(&c);dc_wire_packet p;assert(dc_wire_receive(fd[1],&p)==1&&p.header.type==DC_CMD_GRANT);
 assert(!dc_audio_route_cancel(c.route,old,104));send_microphone(&c);assert(dc_wire_receive(fd[1],&p)==1&&p.header.type==DC_CMD_CANCEL);
 unsigned char frame[320];for(unsigned i=0;i<300;i++){memset(frame,(unsigned char)i,sizeof frame);assert(!dc_audio_route_capture(c.route,frame,sizeof frame,2,1,105));}
 dc_tag next={21,old.reply_generation+1,2};assert(!dc_audio_route_question(c.route,next,106));
 dc_audio_status s;dc_audio_route_status(c.route,&s);assert(s.microphone_bytes==96000&&s.cancel_barrier);send_microphone(&c);assert(dc_wire_receive(fd[1],&p)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
 dc_wire_header event=dc_wire_make(DC_EVT_RESUMED,21,old.reply_generation,8,0);assert(!dc_wire_send(fd[1],&event,NULL));adapter_events(&c,107);assert(!c.failed);
 unsigned bytes=0,commits=0;for(unsigned round=0;round<1000&&!commits;round++){
  send_microphone(&c);while(dc_wire_receive(fd[1],&p)==1){
   if(p.header.type==DC_CMD_MIC){for(unsigned j=0;j<p.header.length;j++)assert(p.payload[j]==(unsigned char)((bytes+j)/320));bytes+=p.header.length;}
   else {assert(p.header.type==DC_CMD_RESUME&&p.header.generation==next.reply_generation);commits++;}
  }
 }
 assert(bytes==96000&&commits==1&&!c.failed);dc_audio_route_destroy(c.route);close(fd[0]);close(fd[1]);
}
int main(void){
 bounded_followup();
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,fd));
 struct core old={0},current={0};int old_closed=0,current_closed=0;
 dc_config cfg={0};dc_session_ops a={&old_closed,noop_stop,noop_cancel,noop_question,closed,noop_light}, b={&current_closed,noop_stop,noop_cancel,noop_question,closed,noop_light};
 assert(!dc_session_open(&old.session,11,&cfg,&a,"",100));assert(!dc_session_ready(&old.session,101));
 assert(!dc_session_open(&current.session,12,&cfg,&b,"",100));assert(!dc_session_ready(&current.session,101));
 old.audio=old.ready=1;old.adapter_fd=fd[0];old.capture=(dc_io_capture *)&old;old.asr=(lx06_control_asr *)&old;old.guard=(dc_voice_guard *)&old;
 old.route=dc_audio_route_create(11,0);assert(old.route);assert(!dc_audio_route_ready(old.route));
 dc_tag tag=dc_session_reply(&old.session,1,102),bound;assert(dc_audio_route_begin(old.route,7,103,&bound)==DC_AUDIO_BEGIN_NEED_TAG);assert(!dc_audio_route_bind(old.route,7,tag,103));send_microphone(&old);
 dc_wire_packet packet;assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_GRANT);
 assert(!dc_audio_route_cancel(old.route,tag,104));send_microphone(&old);assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_CANCEL);
 /* Four seconds of 16 kHz mono s16le fill the real route through capture(). */
 for(unsigned i=0;i<400;i++){feed(&old,105);assert(!old.failed);}
 dc_audio_status status;dc_audio_route_status(old.route,&status);assert(status.cancel_barrier&&status.microphone_bytes==DC_AUDIO_MIC_CAP);
 int logpipe[2];assert(!pipe(logpipe));int saved=dup(STDERR_FILENO);assert(saved>=0);assert(dup2(logpipe[1],STDERR_FILENO)>=0);close(logpipe[1]);
 feed(&old,105);assert(dup2(saved,STDERR_FILENO)>=0);close(saved);char log[512]={0};ssize_t n=read(logpipe[0],log,sizeof log-1);close(logpipe[0]);assert(n>0&&strstr(log,"event=capture_overflow value=105"));
 assert(old.failed);dc_audio_route_status(old.route,&status);assert(status.microphone_bytes==DC_AUDIO_MIC_CAP);
 dc_wire_header resume=dc_wire_make(DC_EVT_RESUMED,11,tag.reply_generation,7,0);assert(!dc_wire_send(fd[1],&resume,NULL));adapter_events(&old,106);send_microphone(&old);
 assert(dc_wire_receive(fd[1],&packet)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
 assert(!dc_audio_route_tick(old.route,15104));assert(dc_audio_route_tick(old.route,15105)==-ETIMEDOUT);
 /* Closing an old in-memory core cannot mutate a separately owned core. This is
  * not a test of the process-global supervisor/lease handoff. */
 assert(dc_session_close(&old.session,106)>0);assert(old_closed==1&&!current_closed);assert(!current.session.control.closed&&current.session.control.tag.session_epoch==12);
 assert(dc_session_reply(&current.session,2,107).session_epoch==12);
 dc_audio_route_destroy(old.route);close(fd[0]);close(fd[1]);puts("production capture overflow: PASS (synthetic capture/ASR/guard; separate core objects only)");return 0;
}
