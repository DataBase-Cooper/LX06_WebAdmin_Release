/* Offline real session -> reactor -> route, IPC and player; synthetic boundaries. */
#define main dc_program_main_unused
#include "../main.c"
#undef main
#include <assert.h>
#include <stdatomic.h>
#ifdef NDEBUG
#error assertions required
#endif
struct sink { atomic_int blocked; atomic_uint attempts, drops, frames; atomic_int bad; };
static int so(void *p,unsigned r){(void)p;(void)r;return 0;}
static int sp(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}
static ssize_t sw(void *p,const int16_t *x,size_t n){struct sink *s=p;atomic_fetch_add(&s->attempts,1);if(atomic_load(&s->blocked))return -EAGAIN;for(size_t i=0;i<n;i++)if(x[i]!=0x3333)atomic_store(&s->bad,1);atomic_fetch_add(&s->frames,(unsigned)n);return (ssize_t)n;}
static int sd(void *p,long *n){(void)p;*n=0;return 0;}
static int s0(void *p){(void)p;return 0;}
static int drop(void *p){atomic_fetch_add(&((struct sink*)p)->drops,1);return 0;}
static void sc(void *p){(void)p;}
static void quiet_light(void *p,dc_tag t,uint64_t s,enum dc_state st){(void)p;(void)t;(void)s;(void)st;}
static void quiet_close(void *p){(void)p;assert(!"unexpected close");}
static void await_value(atomic_uint *v,unsigned value){uint64_t end=now_ms()+3000;while(atomic_load(v)<value&&now_ms()<end)usleep(1000);assert(atomic_load(v)>=value);}
static void await_stop(struct core *c){uint64_t end=now_ms()+3000;while(c->stopping_player&&now_ms()<end){player_events(c,now_ms());usleep(1000);}assert(!c->stopping_player&&!c->failed);}
static void evt(int fd,unsigned type,uint64_t gen,uint64_t seq,const void *data,size_t n){dc_wire_header h=dc_wire_make(type,11,gen,seq,n);if(type==DC_EVT_AUDIO){h.flags=DC_WIRE_PCM_S16LE;h.rate=16000;h.channels=1;}assert(!dc_wire_send(fd,&h,data));}
static dc_wire_packet receive(int fd,unsigned type){dc_wire_packet p;assert(dc_wire_receive(fd,&p)==1);assert(p.header.type==type);return p;}
int main(void){
 int fd[2],size=4096;assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,fd));assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof size));
 struct sink sink={0};dc_io_playback_backend b={&sink,so,sp,sw,sd,s0,drop,sc};struct core c={0};c.adapter_fd=fd[0];c.audio=c.ready=c.audio_playback=1;c.player=dc_io_player_create(&b);assert(c.player);c.route=dc_audio_route_create(11,0);assert(c.route);assert(!dc_audio_route_ready(c.route));
 dc_config cfg={0};strcpy(cfg.interrupt,"等一下,等等,你说得不对");dc_session_ops ops={&c,stop_local,cancel_remote,question,quiet_close,quiet_light};assert(!dc_session_open(&c.session,11,&cfg,&ops,"",100));assert(!dc_session_ready(&c.session,101));
 dc_tag current=dc_session_reply(&c.session,1,102),bound;assert(dc_audio_route_begin(c.route,7,103,&bound)==DC_AUDIO_BEGIN_NEED_TAG);assert(!dc_audio_route_bind(c.route,7,current,103));send_microphone(&c);dc_wire_packet p=receive(fd[1],DC_CMD_GRANT);assert(p.header.sequence==7&&p.header.generation==current.reply_generation);
 int16_t pcm[320];for(unsigned i=0;i<320;i++)pcm[i]=0x1111;
 atomic_store(&sink.blocked,1);assert(!begin_player(&c,current,16000));assert(!dc_io_player_write(c.player,current,pcm,sizeof pcm));await_value(&sink.attempts,1);
 unsigned char junk[1024]={0};unsigned filled=0;while(send(fd[0],junk,sizeof junk,MSG_DONTWAIT)>0)filled++;assert(filled&&(errno==EAGAIN||errno==EWOULDBLOCK));
 for(unsigned round=0;round<2;round++){
  uint64_t seq=7+round,utt=2+round;dc_tag old=current,src={11,old.reply_generation,utt};unsigned char first[320],second[320];memset(first,0x41+round,sizeof first);memset(second,0x51+round,sizeof second);
  assert(!dc_audio_route_capture(c.route,first,sizeof first,utt,1,104));assert(dc_session_text(&c.session,src,"等一下",0,105)==1);assert(c.stopping_player&&!c.player_live&&!c.failed);
  assert(!dc_audio_route_capture(c.route,second,sizeof second,utt,1,106));assert(dc_session_text(&c.session,src,"等一下，完整问题",1,107)==1);current=c.session.control.tag;assert(current.reply_generation!=old.reply_generation);
  dc_audio_status st;dc_audio_route_status(c.route,&st);assert(st.cancel_barrier&&st.microphone_bytes==640&&st.microphone_bytes<=DC_AUDIO_MIC_CAP&&st.turns<=DC_AUDIO_TURNS);
  send_microphone(&c);if(!round){for(unsigned i=0;i<filled;i++)assert(recv(fd[1],junk,sizeof junk,0)==sizeof junk);send_microphone(&c);}p=receive(fd[1],DC_CMD_CANCEL);assert(p.header.epoch==11&&p.header.generation==old.reply_generation&&p.header.sequence==seq);send_microphone(&c);assert(dc_wire_receive(fd[1],&p)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
  await_value(&sink.drops,round+1);await_stop(&c);
  evt(fd[1],DC_EVT_CANCEL_ACK,old.reply_generation,seq,NULL,0);evt(fd[1],DC_EVT_RESUMED,current.reply_generation,seq,NULL,0);evt(fd[1],DC_EVT_RESUMED,old.reply_generation,seq+1,NULL,0);
  /* Cross the previous cancellation's late ACK/resume with this live barrier. */
  if(round){evt(fd[1],DC_EVT_CANCEL_ACK,old.reply_generation-1,seq-1,NULL,0);evt(fd[1],DC_EVT_RESUMED,old.reply_generation-1,seq-1,NULL,0);}
  adapter_events(&c,108);dc_audio_route_status(c.route,&st);assert(!c.failed&&st.cancel_barrier&&st.microphone_bytes==640);send_microphone(&c);assert(dc_wire_receive(fd[1],&p)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
  evt(fd[1],DC_EVT_AUDIO,old.reply_generation,seq,pcm,sizeof pcm);evt(fd[1],DC_EVT_RESUMED,old.reply_generation,seq,NULL,0);adapter_events(&c,109);dc_audio_route_status(c.route,&st);assert(!c.failed&&!st.cancel_barrier&&!c.player_live&&!c.have_audio);
  send_microphone(&c);p=receive(fd[1],DC_CMD_MIC);assert(p.header.length==640&&!memcmp(p.payload,first,320)&&!memcmp(p.payload+320,second,320));p=receive(fd[1],DC_CMD_RESUME);assert(p.header.generation==current.reply_generation);
  evt(fd[1],DC_EVT_REPLY_BEGIN,current.reply_generation,seq+1,NULL,0);adapter_events(&c,110);assert(!c.failed);send_microphone(&c);p=receive(fd[1],DC_CMD_GRANT);assert(p.header.sequence==seq+1&&p.header.generation==current.reply_generation);
  /* A late acknowledgement from the preceding generation must not cancel the new owner. */
  evt(fd[1],DC_EVT_RESUMED,old.reply_generation,seq,NULL,0);evt(fd[1],DC_EVT_CANCEL_ACK,old.reply_generation,seq,NULL,0);evt(fd[1],DC_EVT_AUDIO,old.reply_generation,seq,pcm,sizeof pcm);adapter_events(&c,111);send_microphone(&c);assert(dc_wire_receive(fd[1],&p)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
  if(!round){unsigned attempts=atomic_load(&sink.attempts);atomic_store(&sink.blocked,1);evt(fd[1],DC_EVT_AUDIO,current.reply_generation,seq+1,pcm,sizeof pcm);adapter_events(&c,112);assert(c.player_live&&!c.failed);await_value(&sink.attempts,attempts+1);}
 }
 for(unsigned i=0;i<320;i++){pcm[i]=0x3333;}atomic_store(&sink.blocked,0);evt(fd[1],DC_EVT_AUDIO,current.reply_generation,9,pcm,sizeof pcm);adapter_events(&c,113);assert(!c.failed&&c.player_live);await_value(&sink.frames,320);assert(atomic_load(&sink.frames)==320&&!atomic_load(&sink.bad));
 stop_local(&c,current);await_stop(&c);dc_io_player_destroy(c.player);dc_audio_route_destroy(c.route);close(fd[0]);close(fd[1]);puts("repeated interrupt IPC PASS (synthetic; no latency measurement)");return 0;
}
