#define _POSIX_C_SOURCE 200809L
#include "../protocol.h"
#include "transport.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define OUT_SLOTS 16u
#define HOLD_SLOTS 64u
#define HOLD_BYTES 32768u
struct queued {dc_wire_packet packet;};
struct held {dc_wire_header header;size_t offset;};
struct identity {uint64_t sequence,generation;};
struct worker {
 uint64_t epoch,begun_max;dc_transport *transport;
 struct identity identities[32];unsigned identity_next;
 struct queued out[OUT_SLOTS];struct held hold[HOLD_SLOTS];
 unsigned char hold_data[HOLD_BYTES];
 unsigned out_head,out_used,hold_used;size_t hold_bytes;
 int failed,greet_sent;
};
static volatile sig_atomic_t stopped;
static void stop(int sig){(void)sig;stopped=1;}
/* The worker alone is expendable. An elapsed-time process deadline bounds
 * resolver, TCP, TLS, upgrade and provider session-ready, including libraries
 * which may retry EINTR. It cannot stop the core/player/control thread. */
static void connect_expired(int sig){(void)sig;_exit(124);}
static int connect_timer(unsigned ms){struct itimerval timer;memset(&timer,0,sizeof timer);timer.it_value.tv_sec=ms/1000;timer.it_value.tv_usec=(ms%1000)*1000;return setitimer(ITIMER_REAL,&timer,NULL);}
static const char *setting(const char *name,const char *fallback){const char *s=getenv(name);return s&&*s?s:fallback;}
static int bounded(const char *name,unsigned fallback,unsigned low,unsigned high,int zero,unsigned *out){
 const char *s=getenv(name);if(!s){*out=fallback;return 0;}if(!*s)return -1;
 unsigned n=0;for(;*s;s++){if(*s<'0'||*s>'9'||n>(high-(unsigned)(*s-'0'))/10u)return -1;n=n*10u+(unsigned)(*s-'0');}
 if((!n&&zero)||(n>=low&&n<=high)){*out=n;return 0;}return -1;
}
static uint64_t generation(struct worker *w,uint64_t sequence){
 for(unsigned i=0;i<32;i++)if(w->identities[i].sequence==sequence)return w->identities[i].generation;
 return 0;
}
static int enqueue(struct worker *w,const dc_wire_header *h,const void *data){
 if(w->out_used==OUT_SLOTS)return -1;
 struct queued *q=&w->out[(w->out_head+w->out_used)%OUT_SLOTS];q->packet.header=*h;
 if(h->length)memcpy(q->packet.payload,data,h->length);
 w->out_used++;return 0;
}
static int flush(struct worker *w){
 while(w->out_used){
  dc_wire_packet *p=&w->out[w->out_head].packet;
  if(dc_wire_send(DC_ADAPTER_FD,&p->header,p->payload))return errno==EAGAIN||errno==EWOULDBLOCK?0:-1;
  w->out_head=(w->out_head+1)%OUT_SLOTS;w->out_used--;
 }
 return 0;
}
static int publish(struct worker *w,unsigned type,uint64_t sequence,const void *data,size_t n,unsigned rate,unsigned flags){
 do {
  size_t take=n>DC_WIRE_CAP?DC_WIRE_CAP:n;
  if(type==DC_EVT_TEXT&&n>take){while(take&&(((const unsigned char *)data)[take]&0xc0)==0x80)take--;if(!take)return -1;}
  dc_wire_header h=dc_wire_make(type,w->epoch,generation(w,sequence),sequence,take);
  h.flags=flags;if(type==DC_EVT_AUDIO){h.rate=rate;h.channels=1;}
  if(sequence&&type!=DC_EVT_REPLY_BEGIN&&!h.generation&&
     (type==DC_EVT_AUDIO||type==DC_EVT_TEXT||type==DC_EVT_REPLY_END)){
   if(w->hold_used==HOLD_SLOTS||take>HOLD_BYTES-w->hold_bytes)return -1;
   struct held *q=&w->hold[w->hold_used++];q->header=h;q->offset=w->hold_bytes;
   if(take)memcpy(w->hold_data+q->offset,data,take);
   w->hold_bytes+=take;
  }else if(enqueue(w,&h,data))return -1;
  if(take){data=(const unsigned char *)data+take;n-=take;}
  if(!n)break;
 }while(1);
 return flush(w);
}
static int emitted(void *opaque,const struct dc_transport_event *e){
 struct worker *w=opaque;unsigned type,flags=0;
 switch(e->type){
 case DC_T_READY:if(connect_timer(0))return -1;type=DC_EVT_READY;break;
 case DC_T_REPLY_BEGIN:type=DC_EVT_REPLY_BEGIN;w->begun_max=e->reply_sequence;break;
 case DC_T_PCM:type=DC_EVT_AUDIO;flags=DC_WIRE_PCM_S16LE;break;
 case DC_T_ASSISTANT_TEXT:type=DC_EVT_TEXT;break;
 case DC_T_USER_PARTIAL:type=DC_EVT_USER_PARTIAL;break;
 case DC_T_USER_FINAL:type=DC_EVT_USER_FINAL;flags=DC_WIRE_FINAL;break;
 case DC_T_REPLY_END:type=DC_EVT_REPLY_END;break;
 case DC_T_CANCEL_ACK:type=DC_EVT_CANCEL_ACK;break;
 case DC_T_LISTEN_READY:type=DC_EVT_RESUMED;break;
 case DC_T_ERROR:type=DC_EVT_ERROR;break;
 /* The core owns human/keyword decisions. Provider VAD is not a control. */
 case DC_T_SPEECH_STARTED:return 0;
 case DC_T_REMOTE_CLOSE:w->failed=1;return 0;
 default:return -1;
 }
 int result=publish(w,type,e->reply_sequence,e->data,e->bytes,e->sample_rate,flags);
 if(result)w->failed=1;
 return result;
}
static int release_held(struct worker *w){
 unsigned retained=0;size_t held=0;
 for(unsigned i=0;i<w->hold_used;i++){
  struct held p=w->hold[i];uint64_t gen=generation(w,p.header.sequence);
  if(gen&&w->out_used<OUT_SLOTS){p.header.generation=gen;if(enqueue(w,&p.header,w->hold_data+p.offset))return -1;if(flush(w))return -1;}
  else {memmove(w->hold_data+held,w->hold_data+p.offset,p.header.length);p.offset=held;w->hold[retained++]=p;held+=p.header.length;}
 }
 w->hold_used=retained;w->hold_bytes=held;return flush(w);
}
static int remember(struct worker *w,const dc_wire_header *h){
 if(!h->sequence||!h->generation||h->sequence>w->begun_max)return -1;
 uint64_t old=generation(w,h->sequence);if(old&&old!=h->generation)return -1;
 if(!old){struct identity *id=&w->identities[w->identity_next++%32];id->sequence=h->sequence;id->generation=h->generation;}
 return 0;
}
static int grant(struct worker *w,const dc_wire_header *h){return remember(w,h)?-1:release_held(w);}
static int cancel(struct worker *w,const dc_wire_header *h){
 if(remember(w,h))return -1;
 /* A pre-BEGIN tombstone intentionally has no playback grant. Keep its
  * core identity for RESUMED, but never flush its buffered old output. */
 unsigned kept=0;size_t bytes=0;
 for(unsigned i=0;i<w->hold_used;i++){
  struct held p=w->hold[i];if(p.header.sequence==h->sequence)continue;
  memmove(w->hold_data+bytes,w->hold_data+p.offset,p.header.length);p.offset=bytes;w->hold[kept++]=p;bytes+=p.header.length;
 }
 w->hold_used=kept;w->hold_bytes=bytes;
 return dc_transport_cancel(w->transport,h->sequence);
}
static int configure(const char *provider,struct dc_transport_config *c){
 memset(c,0,sizeof(*c));
 if(!strcmp(provider,"xiaozhi")){
  c->provider=DC_TRANSPORT_XIAOZHI;c->endpoint=setting("DC_ENDPOINT",setting("XZ_WS_URL",""));
  c->device_id=setting("DC_DEVICE_ID",setting("XZ_DEVICE_ID",""));c->client_id=setting("DC_CLIENT_ID",setting("XZ_CLIENT_ID",""));
  c->credential=setting("DC_CREDENTIAL",setting("XZ_ACCESS_TOKEN",""));
 }else if(!strcmp(provider,"doubao")){
  c->provider=DC_TRANSPORT_DOUBAO;c->endpoint=setting("DC_ENDPOINT",setting("DB_ENDPOINT","wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"));
  c->credential=setting("DC_CREDENTIAL",setting("DB_API_KEY",""));
  c->voice=setting("DB_VOICE",NULL);c->instructions=setting("DB_INSTRUCTIONS",NULL);
  c->output_pcm=!strcmp(setting("DB_AUDIO_FORMAT","ogg_opus"),"pcm_s16le");
 }else return -1;
 const char *mode=setting("DC_UPLINK_MODE","guarded");
 if(strcmp(mode,"guarded")&&strcmp(mode,"native"))return -1;
 c->native_realtime=!strcmp(mode,"native");
 if(bounded(c->provider==DC_TRANSPORT_XIAOZHI?"XZ_CONNECT_TIMEOUT_MS":"DB_CONNECT_TIMEOUT_MS",c->provider==DC_TRANSPORT_XIAOZHI?10000:8000,1000,60000,0,&c->connect_timeout_ms))return -1;
 if(c->provider==DC_TRANSPORT_XIAOZHI&&
    (bounded("XZ_PING_INTERVAL_MS",15000,100,300000,1,&c->ping_interval_ms)||bounded("XZ_PONG_TIMEOUT_MS",10000,100,60000,0,&c->pong_timeout_ms)))return -1;
 return 0;
}
static uint64_t sequence_for_generation(struct worker *w,uint64_t gen){
 for(unsigned i=0;i<32;i++)if(w->identities[i].generation==gen)return w->identities[i].sequence;
 return 0;
}
struct check_state {int ready,failed;};
static int check_event(void *opaque,const struct dc_transport_event *event){
 struct check_state *check=opaque;
 if(event->type==DC_T_READY){check->ready=1;return connect_timer(0);}
 /* This extension action never starts a turn, sends MIC or accepts output.
  * TCP/TLS/WS success alone is not proof of authenticated session readiness. */
 check->failed=1;return -1;
}
static uint64_t milliseconds(void){
 struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
 return (uint64_t)now.tv_sec*1000u+(unsigned long)now.tv_nsec/1000000u;
}
static int connection_check(const struct dc_transport_config *config){
 struct check_state check={0};if(connect_timer(config->connect_timeout_ms))return 1;
 dc_transport *transport=dc_transport_open(config,check_event,&check);
 uint64_t deadline=milliseconds()+config->connect_timeout_ms;
 while(transport&&!stopped&&!check.ready&&!check.failed&&milliseconds()<deadline){
  if(dc_transport_poll(transport,100)){check.failed=1;break;}
 }
 int result=!transport||stopped||!check.ready||check.failed;
 dc_transport_close(transport);
 puts(result?"transport: connection check failed":"transport: authenticated session ready");
 return result;
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--protocol")){puts("LX06DCTRANSPORT/1");return 0;}
 signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);signal(SIGALRM,connect_expired);
 if(argc==4&&!strcmp(argv[1],"--check")&&!strcmp(argv[2],"--provider")&&!strcmp(argv[3],"doubao")){
  struct dc_transport_config config;if(configure(argv[3],&config))return 2;
  return connection_check(&config);
 }
 if(argc!=3||strcmp(argv[1],"--provider"))return 2;
 struct dc_transport_config config;if(configure(argv[2],&config))return 2;
 int kind=0;socklen_t length=sizeof(kind);
 if(getsockopt(DC_ADAPTER_FD,SOL_SOCKET,SO_TYPE,&kind,&length)||kind!=SOCK_SEQPACKET)return 2;
 struct worker *w=calloc(1,sizeof(*w));if(!w)return 1;
 int result=0,opened=0;
 while(!stopped&&!w->failed){
  if(flush(w)||release_held(w)){result=1;break;}
  dc_wire_packet p;int received=dc_wire_receive(DC_ADAPTER_FD,&p);
  if(received==0)break;
  if(received<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK){result=1;break;}
  if(received>0){
   dc_wire_header *h=&p.header;
   if(h->type==DC_CMD_OPEN){
    if(opened||!h->epoch||h->generation||h->sequence||h->length){result=2;break;}
    opened=1;w->epoch=h->epoch;if(connect_timer(config.connect_timeout_ms)){result=1;break;}w->transport=dc_transport_open(&config,emitted,w);
    if(!w->transport){(void)publish(w,DC_EVT_ERROR,0,NULL,0,0,0);result=1;break;}
   }else {
    if(!opened||h->epoch!=w->epoch){result=2;break;}
    int status=0;
    switch(h->type){
    case DC_CMD_GREET:{
     if(w->greet_sent||w->begun_max||!h->generation||h->sequence||h->flags||!h->length||h->length>240||memchr(p.payload,0,h->length)){status=-1;break;}
     char text[241];memcpy(text,p.payload,h->length);text[h->length]=0;
     status=dc_transport_text(w->transport,text);if(!status)w->greet_sent=1;break;
    }
    case DC_CMD_MIC:
     if(h->rate!=16000||h->channels!=1)status=-1;
     else status=dc_transport_pcm(w->transport,p.payload,h->length);
     break;
    case DC_CMD_GRANT:status=h->length?-1:grant(w,h);break;
    case DC_CMD_CANCEL:{
     uint64_t seq=h->sequence?h->sequence:sequence_for_generation(w,h->generation);
     dc_wire_header cancellation=*h;cancellation.sequence=seq;
     status=h->length?-1:cancel(w,&cancellation);break;
    }
    case DC_CMD_RESUME:
     /* The sole core recognizer may attach its complete QUESTION FINAL for
      * observability. Audio providers already received that utterance as PCM;
      * forwarding it as text as well would submit the user's question twice. */
     if(h->length&&(!(h->flags&DC_WIRE_FINAL)||h->length>=2048||memchr(p.payload,0,h->length)))status=-1;
     else status=h->flags&DC_WIRE_FINAL?dc_transport_commit(w->transport):dc_transport_listen(w->transport);
     break;
    case DC_CMD_CLOSE:stopped=1;break;
    default:status=-1;break;
    }
    if(status){result=1;break;}
   }
  }
  if(w->transport&&!w->out_used){int polled=dc_transport_poll(w->transport,0);if(polled){result=polled<0?1:0;break;}}
  struct pollfd wait={DC_ADAPTER_FD,(short)(POLLIN|(w->out_used?POLLOUT:0)),0};
  (void)poll(&wait,1,2);
 }
 if(w->failed)result=1;
 (void)flush(w);dc_transport_close(w->transport);free(w);return result;
}
