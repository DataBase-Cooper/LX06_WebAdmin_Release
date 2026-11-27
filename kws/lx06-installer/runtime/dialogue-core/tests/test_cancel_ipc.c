/* Offline production session/reactor/player/route; synthetic local adapter. */
#define main dc_program_main_unused
#include "../main.c"
#undef main
#include <assert.h>
#include <stdatomic.h>
#ifdef NDEBUG
#error assertions required
#endif
struct sink {atomic_int attempted,drop;};
static int so(void *p,unsigned r){(void)p;(void)r;return 0;}
static int sp(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}
static ssize_t sw(void *p,const int16_t *x,size_t n){(void)x;(void)n;atomic_store(&((struct sink*)p)->attempted,1);return -EAGAIN;}
static int sd(void *p,long *n){(void)p;*n=0;return 0;}
static int s0(void *p){(void)p;return 0;}
static int drop(void *p){atomic_store(&((struct sink*)p)->drop,1);return 0;}
static void sc(void *p){(void)p;}
static void quiet_light(void *p,dc_tag t,uint64_t s,enum dc_state st){(void)p;(void)t;(void)s;(void)st;}
static void quiet_close(void *p){(void)p;assert(!"unexpected close");}
int main(void){
 int fd[2],size=4096;assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,fd));assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof size));
 struct sink sink={0};dc_io_playback_backend b={&sink,so,sp,sw,sd,s0,drop,sc};
 struct core c={0};c.adapter_fd=fd[0];c.audio=c.ready=1;c.player=dc_io_player_create(&b);assert(c.player);c.route=dc_audio_route_create(11,0);assert(c.route);assert(!dc_audio_route_ready(c.route));
 dc_config cfg={0};strcpy(cfg.interrupt,"等一下,等等,你说得不对");strcpy(cfg.goodbye,"结束对话,拜拜了哦");
 dc_session_ops ops={&c,stop_local,cancel_remote,question,quiet_close,quiet_light};assert(!dc_session_open(&c.session,11,&cfg,&ops,"",100));assert(!dc_session_ready(&c.session,101));dc_tag old=dc_session_reply(&c.session,1,102),got;
 assert(dc_audio_route_begin(c.route,7,103,&got)==DC_AUDIO_BEGIN_NEED_TAG);assert(!dc_audio_route_bind(c.route,7,old,103));send_microphone(&c);
 dc_wire_packet packet;assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_GRANT);
 int16_t audio[320]={0};assert(!begin_player(&c,old,16000));assert(!dc_io_player_write(c.player,old,audio,sizeof audio));uint64_t end=now_ms()+3000;while(!atomic_load(&sink.attempted)&&now_ms()<end)usleep(1000);assert(atomic_load(&sink.attempted));
 unsigned char junk[1024]={0};unsigned filled=0;while(send(fd[0],junk,sizeof junk,MSG_DONTWAIT)>0)filled++;assert(filled&& (errno==EAGAIN||errno==EWOULDBLOCK));
 unsigned char first[320],second[320];memset(first,0x31,sizeof first);memset(second,0x42,sizeof second);
 assert(!dc_audio_route_capture(c.route,first,sizeof first,2,1,104));dc_tag src={11,old.reply_generation,2};assert(dc_session_text(&c.session,src,"等一下",0,105)==1);assert(c.stopping_player&&!c.player_live&&!c.failed);
 assert(!dc_audio_route_capture(c.route,second,sizeof second,2,1,106));assert(dc_session_text(&c.session,src,"等一下，完整问题",1,107)==1);dc_tag next=c.session.control.tag;
 dc_audio_status status;dc_audio_route_status(c.route,&status);assert(status.cancel_barrier&&status.microphone_bytes==640&&status.turns==1);send_microphone(&c);dc_audio_route_status(c.route,&status);assert(status.microphone_bytes==640&&!c.failed);
 for(unsigned i=0;i<filled;i++)assert(recv(fd[1],junk,sizeof junk,0)==sizeof junk);
 send_microphone(&c);assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_CANCEL&&packet.header.sequence==7&&packet.header.generation==old.reply_generation);send_microphone(&c);assert(dc_wire_receive(fd[1],&packet)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
 end=now_ms()+3000;while(!atomic_load(&sink.drop)&&now_ms()<end)usleep(1000);assert(atomic_load(&sink.drop));
 /* No remote acknowledgement: retained capture is bounded and no upload leaks. */
 unsigned added=0;while(!dc_audio_route_capture(c.route,junk,320,2,1,108))added+=320;dc_audio_route_status(c.route,&status);assert(status.microphone_bytes==DC_AUDIO_MIC_CAP&&added==DC_AUDIO_MIC_CAP-640);assert(status.turns<=DC_AUDIO_TURNS);assert(dc_audio_route_capture(c.route,junk,320,2,1,108)==-ENOBUFS);
 /* ACK and wrong-identity RESUMED must not release the barrier. */
 dc_wire_header event=dc_wire_make(DC_EVT_CANCEL_ACK,11,old.reply_generation,7,0);assert(!dc_wire_send(fd[1],&event,NULL));
 event=dc_wire_make(DC_EVT_RESUMED,11,old.reply_generation+1,7,0);assert(!dc_wire_send(fd[1],&event,NULL));
 event=dc_wire_make(DC_EVT_RESUMED,11,old.reply_generation,8,0);assert(!dc_wire_send(fd[1],&event,NULL));
 adapter_events(&c,109);dc_audio_route_status(c.route,&status);assert(!c.failed&&status.cancel_barrier&&status.microphone_bytes==DC_AUDIO_MIC_CAP);send_microphone(&c);assert(dc_wire_receive(fd[1],&packet)==-1&&(errno==EAGAIN||errno==EWOULDBLOCK));
 /* Resume travels through the same socket and dispatcher as production. */
 event=dc_wire_make(DC_EVT_RESUMED,11,old.reply_generation,7,0);assert(!dc_wire_send(fd[1],&event,NULL));adapter_events(&c,110);dc_audio_route_status(c.route,&status);assert(!c.failed&&!status.cancel_barrier);
 /* Old generation output and duplicate resume cannot affect the new capture. */
 event=dc_wire_make(DC_EVT_REPLY_BEGIN,11,old.reply_generation,7,0);assert(!dc_wire_send(fd[1],&event,NULL));
 c.audio_playback=1;event=dc_wire_make(DC_EVT_AUDIO,11,old.reply_generation,7,sizeof audio);event.flags=DC_WIRE_PCM_S16LE;event.rate=16000;event.channels=1;assert(!dc_wire_send(fd[1],&event,audio));
 event=dc_wire_make(DC_EVT_REPLY_END,11,old.reply_generation,7,0);assert(!dc_wire_send(fd[1],&event,NULL));
 event=dc_wire_make(DC_EVT_RESUMED,11,old.reply_generation,7,0);assert(!dc_wire_send(fd[1],&event,NULL));adapter_events(&c,111);assert(!c.failed&&!c.player_live&&!c.have_audio&&!c.account_done);
 /* Only the finalized watermark should upload after remote resume. */
 send_microphone(&c);assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_MIC&&packet.header.length==640);assert(!memcmp(packet.payload,first,320)&&!memcmp(packet.payload+320,second,320));assert(dc_wire_receive(fd[1],&packet)==1&&packet.header.type==DC_CMD_RESUME&&packet.header.generation==next.reply_generation);
 end=now_ms()+3000;while(!atomic_load(&sink.drop)&&now_ms()<end)usleep(1000);assert(atomic_load(&sink.drop));dc_io_player_destroy(c.player);dc_audio_route_destroy(c.route);close(fd[0]);close(fd[1]);printf("cancel IPC PASS: socket full=%u packets; retained=640; bounded=%u; overflow=ENOBUFS\n",filled,DC_AUDIO_MIC_CAP);return 0;
}
