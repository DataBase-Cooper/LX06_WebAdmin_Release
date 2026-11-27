#define _GNU_SOURCE
/* Shared Xiaomi ASR transport: CONTROL for Doubao/Xiaozhi, opt-in QUESTION
 * for account models. QUESTION may reserve a bounded native-TTS window with
 * full Opus replay; CONTROL allocates no archive and retains its behavior.
 * Recognition uses this speaker's official Engine authentication; it is cloud
 * ASR, not an offline recognizer. Partial text binds an utterance to the reply
 * active when speech began. poll() returns only finals; QUESTION callers can
 * hint configured controls to retain their released stable-final endpoint
 * even under playback noise, without accepting provisional words.
 * Preserve that reply tag across delayed finals so an old interruption cannot
 * cancel a new answer. Callers give goodbye matching priority over interruption.
 */
#include "control_asr.h"
#include "../doubao/src/json.h"
#include <opus/opus.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

struct lx06_control_asr {
 int fd,protect,active,ended,final_seen,cloud_vad,profile,tts_held;
 uint32_t epoch;
 uint64_t started,finish_at,last_ping,first_partial,last_partial,partial_changed,ready_after,tts_hold_until,priority_until;
 uint64_t reply,utterance_reply,result_reply;
 uint64_t replay_first_partial,replay_reply;
 uint64_t stable_ms,utterance_ms,turn_ms;
 uint64_t last_pcm_at;
 uint64_t window_start_ms,window_max_ms,speech_started;
 unsigned quiet_ms;
 int pcm_seen,control_hint,speech_seen,window_registered;
 char partial[2048];
 char dialog[96];
 unsigned char pcm[640];size_t used;
 unsigned char pending[64000];size_t pending_used,pending_dropped;
 unsigned char *question_pending;size_t pending_head;int pending_growth_failed;
 unsigned char *archive;size_t archive_used,replay_at;
 int priority,archive_complete,replay_pending;
 unsigned char *outbox;size_t outbox_at,outbox_used;
 OpusEncoder *encoder;
 xz_json_doc doc;
 char message[65536];
};
#define LX06_QUESTION_QUIET_PEAK 32U
#define LX06_QUESTION_QUIET_MEAN 8U
#define LX06_QUESTION_QUIET_MS 300U
#define LX06_QUESTION_PCM_FRESH_MS 120U
#define LX06_ASR_ARCHIVE_CAP (192U*1024U)
#define LX06_ASR_QUESTION_PENDING_CAP 128000U

static int question_pcm_quiet(const unsigned char *pcm,size_t n) {
 unsigned long long sum=0;unsigned peak=0;size_t samples=n/2;
 if(!samples)return 0;
 for(size_t i=0;i<samples;i++) {
  int16_t sample=(int16_t)((unsigned)pcm[i*2]|((unsigned)pcm[i*2+1]<<8));
  unsigned level=(unsigned)(sample<0?-(int)sample:sample);
  if(level>peak)peak=level;
  sum+=level;
 }
 return peak<=LX06_QUESTION_QUIET_PEAK &&
        sum<=((unsigned long long)samples*LX06_QUESTION_QUIET_MEAN);
}

static void question_note_pcm(struct lx06_control_asr *a,const unsigned char *pcm,
                              size_t n,uint64_t now) {
 int quiet=question_pcm_quiet(pcm,n);a->pcm_seen=1;
 if(!quiet)a->quiet_ms=0;
 else if(!a->quiet_ms || now<=a->last_pcm_at || now-a->last_pcm_at>100)a->quiet_ms=20;
 else {uint64_t delta=now-a->last_pcm_at;if(delta>40)delta=40;
       a->quiet_ms=(unsigned)((a->quiet_ms+delta>1000)?1000:a->quiet_ms+delta);}
 a->last_pcm_at=now;
}

static int question_quiet_ready(const struct lx06_control_asr *a,uint64_t now) {
 return a->pcm_seen&&a->quiet_ms>=LX06_QUESTION_QUIET_MS&&
        now>=a->last_pcm_at&&now-a->last_pcm_at<=LX06_QUESTION_PCM_FRESH_MS;
}
void lx06_control_asr_reply(lx06_control_asr *a,uint64_t reply) {if(a)a->reply=reply;}
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *a) {return a?a->result_reply:0;}
const char *lx06_control_asr_partial(const lx06_control_asr *a,uint64_t *reply) {
 if(reply)*reply=a?a->utterance_reply:0;
 return a&&a->active&&a->first_partial?a->partial:"";
}
static void trace(const char *event,uint64_t now) {
 const char *on=getenv("LX06_CONTROL_ASR_TRACE");
 if(on && !strcmp(on,"1"))fprintf(stderr,"control-asr: %s at=%llu\n",event,(unsigned long long)now);
}
/* A native SDK post can briefly block its IPC reader during TTS. Retain
 * encoded packets exactly once instead of treating EAGAIN as a disconnect.
 * Commands and audio share one ordered, bounded queue; allocate only on the
 * first actual QUESTION backpressure event. CONTROL keeps its released path. */
#define LX06_ASR_OUTBOX_CAP 32768U
static int flush_outbox(lx06_control_asr *a) {
 for(int budget=0;budget<32&&a->outbox_at<a->outbox_used;budget++){
  const unsigned char*p=a->outbox+a->outbox_at;size_t n=((size_t)p[0]<<8)+p[1];
  ssize_t sent=send(a->fd,p+2,n,MSG_DONTWAIT|MSG_NOSIGNAL);
  if(sent<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;
  if(sent!=(ssize_t)n)return -1;
  a->outbox_at+=n+2;
 }
 if(a->outbox_at==a->outbox_used){a->outbox_at=a->outbox_used=0;return 1;}
 return 0;
}
static int send_packet(lx06_control_asr *a,const void *data,size_t n) {
 if(a->profile==LX06_ASR_CONTROL)
  return send(a->fd,data,n,MSG_DONTWAIT|MSG_NOSIGNAL)==(ssize_t)n?0:-1;
 if(!n||n>4096){errno=EMSGSIZE;return -1;}
 int flushed=flush_outbox(a);if(flushed<0)return -1;
 if(flushed){
  ssize_t sent=send(a->fd,data,n,MSG_DONTWAIT|MSG_NOSIGNAL);
  if(sent==(ssize_t)n)return 0;
  if(sent>=0||!(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return -1;
 }
 if(!a->outbox){a->outbox=malloc(LX06_ASR_OUTBOX_CAP);if(!a->outbox)return -1;}
 if(a->outbox_at){memmove(a->outbox,a->outbox+a->outbox_at,a->outbox_used-a->outbox_at);a->outbox_used-=a->outbox_at;a->outbox_at=0;}
 if(n+2>LX06_ASR_OUTBOX_CAP-a->outbox_used){errno=ENOBUFS;return -1;}
 unsigned char*p=a->outbox+a->outbox_used;p[0]=(unsigned char)(n>>8);p[1]=(unsigned char)n;memcpy(p+2,data,n);a->outbox_used+=n+2;
 return 0;
}
static size_t pending_capacity(const lx06_control_asr *a) {
 return a->question_pending?LX06_ASR_QUESTION_PENDING_CAP:sizeof(a->pending);
}
/* QUESTION alone may add one 64,000-byte bank when waiting for a FINAL needs more
 * than the released two-second buffer. Its ring keeps the newest four seconds
 * without allocating a second full 128,000-byte copy or moving audio every frame.
 * CONTROL retains its inline capacity, copy/drop path and per-packet sends. */
static unsigned char *pending_span(lx06_control_asr *a,size_t at,size_t *available) {
 if(at<sizeof(a->pending)){*available=sizeof(a->pending)-at;return a->pending+at;}
 *available=LX06_ASR_QUESTION_PENDING_CAP-at;
 return a->question_pending+(at-sizeof(a->pending));
}
static void defer_audio(lx06_control_asr *a,const void *pcm,size_t n) {
 if(a->profile==LX06_ASR_QUESTION && !a->question_pending && !a->pending_growth_failed &&
    n>sizeof(a->pending)-a->pending_used) {
  a->question_pending=malloc(LX06_ASR_QUESTION_PENDING_CAP-sizeof(a->pending));
  if(!a->question_pending)a->pending_growth_failed=1;
 }
 size_t capacity=pending_capacity(a);
 size_t drop=a->pending_used+n>capacity?a->pending_used+n-capacity:0;
 a->pending_dropped+=drop;
 if(!a->question_pending) {
  if(drop){memmove(a->pending,a->pending+drop,a->pending_used-drop);a->pending_used-=drop;}
  if(n)memcpy(a->pending+a->pending_used,pcm,n);
  a->pending_used+=n;return;
 }
 a->pending_head=(a->pending_head+drop)%capacity;a->pending_used-=drop;
 size_t at=(a->pending_head+a->pending_used)%capacity;const unsigned char *source=pcm;
 a->pending_used+=n;
 while(n) {
  size_t take;unsigned char *dest=pending_span(a,at,&take);if(take>n)take=n;
  memcpy(dest,source,take);source+=take;n-=take;at=(at+take)%capacity;
 }
}
int lx06_control_asr_tts_priority_enable(lx06_control_asr *a) {
 if(!a || a->profile!=LX06_ASR_QUESTION || a->active)return 0;
 if(!a->archive){a->archive=malloc(LX06_ASR_ARCHIVE_CAP);if(!a->archive)return -1;}
 a->priority=1;a->archive_complete=1;return 1;
}
int lx06_control_asr_tts_priority(lx06_control_asr *a,uint64_t now,unsigned hold_ms) {
 if(!a || !a->priority || a->profile!=LX06_ASR_QUESTION || !a->protect || a->tts_held ||
    (a->replay_pending && a->active) || a->pending_used>32000 || !hold_ms || hold_ms>500 || now>UINT64_MAX-hold_ms ||
    (a->priority_until && now>=a->priority_until) ||
    (a->active && (a->ended || !a->archive_complete)))return 0;
 if(a->active) {
  /* No source speech is discarded: the complete original encoded stream stays
   * available for the replacement recognizer, including low-level speech. */
  if(send_packet(a,"C",1))return -1;
  /* Partial text is provisional, not an accepted question/control. Replay
   * its original audio too, retaining its original reply ownership so a
   * delayed control cannot cancel a newer answer after resynthesis. */
  if(a->first_partial){a->replay_first_partial=a->first_partial;a->replay_reply=a->utterance_reply;}
  a->active=0;a->dialog[0]=0;a->replay_pending=1;a->replay_at=0;
 }
 /* A prefetched sentence may start in the same loop as the previous
  * synthesis completes, before replay starts. Reserve that remaining window
  * too, without canceling twice or extending the original 500ms deadline.
  * Once replay has started it must drain before another handoff is allowed. */
 if(!a->priority_until)a->priority_until=now+hold_ms;
 a->tts_held=1;a->tts_hold_until=now+hold_ms;
 if(a->tts_hold_until>a->priority_until)a->tts_hold_until=a->priority_until;
 trace("tts-priority",now);return 1;
}
void lx06_control_asr_control_hint(lx06_control_asr *a,int matched) {
 if(a && a->profile==LX06_ASR_QUESTION)a->control_hint=!!matched;
}
static int replay_flush(lx06_control_asr *a) {
 if(!a->replay_pending)return 1;
 int queued=flush_outbox(a);if(queued<=0)return queued;
 /* Drain in bounded batches; a slow SDK reader must not turn an IPC send
  * buffer limit into a disconnected voice session or block microphone reads. */
 for(int budget=0;budget<32 && a->replay_at<a->archive_used;budget++) {
  const unsigned char *p=a->archive+a->replay_at;
  size_t size=3+((size_t)p[1]<<8)+p[2];
  if(size>a->archive_used-a->replay_at)return -1;
  ssize_t n=send(a->fd,p,size,MSG_DONTWAIT|MSG_NOSIGNAL);
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;
  if(n!=(ssize_t)size)return -1;
  a->replay_at+=size;
 }
 if(a->replay_at<a->archive_used)return 0;
 a->replay_pending=0;a->replay_at=0;a->priority_until=0;return 1;
}
int lx06_control_asr_tts_yield(lx06_control_asr *a,uint64_t now,unsigned hold_ms,uint64_t quiet_since) {
 if(!a || a->profile!=LX06_ASR_QUESTION || !a->protect || a->tts_held ||
    !hold_ms || hold_ms>1000 || now>UINT64_MAX-hold_ms ||
    !quiet_since || now<quiet_since || now-quiet_since<300 ||
    a->pending_used || (a->active &&
    (a->first_partial || a->ended || a->started<quiet_since)))return 0;
 if(a->active) {
  if(send_packet(a,"E",1))return -1;
  a->ended=1;a->finish_at=now;
  /* Only already-confirmed silence can be here. Keep an incomplete PCM
   * frame too; the next encoder must not silently discard caller input. */
  if(a->used){defer_audio(a,a->pcm,a->used);a->used=0;}
 }
 a->tts_held=1;a->tts_hold_until=now+hold_ms;
 trace("tts-yield",now);
 return 1;
}
int lx06_control_asr_tts_resume(lx06_control_asr *a,uint64_t now) {
 if(!a)return -1;
 if(a->profile!=LX06_ASR_QUESTION || !a->tts_held)return 0;
 /* An unexpected late partial is evidence that the turn was not empty.
  * Never cancel it: preserve its final and the ordinary pending-PCM path. */
 if(a->active && !a->first_partial) {
  if(send_packet(a,"C",1))return -1;
  a->active=0;a->dialog[0]=0;
 }
 a->tts_held=0;a->tts_hold_until=0;
 if(!a->active)a->ready_after=now;
 trace("tts-resume",now);
 return 0;
}
static int start(lx06_control_asr *a,uint64_t now) {
 unsigned char request[5]={'V'};
 request[0]=a->cloud_vad?'V':'S';
 a->epoch=(a->epoch%2147483646U)+1;
 for(int i=0;i<4;i++)request[i+1]=(unsigned char)(a->epoch>>(i*8));
 a->dialog[0]=0;a->ended=a->final_seen=0;a->started=now;a->finish_at=0;
 if(!a->replay_pending){a->used=0;a->archive_used=0;a->archive_complete=1;a->priority_until=0;a->replay_first_partial=a->replay_reply=0;}
 a->first_partial=a->last_partial=a->partial_changed=0;a->partial[0]=0;a->control_hint=0;
 a->last_pcm_at=0;a->quiet_ms=0;a->pcm_seen=0;
 if(a->profile==LX06_ASR_WINDOW){a->speech_seen=0;a->speech_started=0;}
 a->active=1;
 trace("start",now);
 /* Replaying the full turn starts a fresh cloud decoder at the same initial
  * state. Preserve our encoder state for the continuation after that replay. */
 if(!a->replay_pending)(void)opus_encoder_ctl(a->encoder,OPUS_RESET_STATE);
 return send_packet(a,request,sizeof(request));
}
lx06_control_asr *lx06_control_asr_open(const char *path) {
 return lx06_control_asr_open_profile(path,LX06_ASR_CONTROL);
}
static int window_readers;
int lx06_control_asr_window_readers(void) { return window_readers; }
void lx06_control_asr_window_ms(lx06_control_asr *a, unsigned ms) {
 if(!a || a->profile!=LX06_ASR_WINDOW) return;
 if(!ms) ms=10000;
 if(ms<3000) ms=3000;
 if(ms>15000) ms=15000;
 a->window_max_ms=ms;
}
void lx06_control_asr_window_timing(lx06_control_asr *a,unsigned start_ms,unsigned max_ms) {
 if(!a || a->profile!=LX06_ASR_WINDOW) return;
 if(!start_ms) start_ms=5000;
 if(start_ms<1000) start_ms=1000;
 if(start_ms>15000) start_ms=15000;
 a->window_start_ms=start_ms;
 lx06_control_asr_window_ms(a,max_ms);
}
int lx06_control_asr_speech_seen(const lx06_control_asr *a,uint64_t *started) {
 if(!a || a->profile!=LX06_ASR_WINDOW || !a->speech_seen) return 0;
 if(started)*started=a->speech_started;
 return 1;
}
lx06_control_asr *lx06_control_asr_open_profile(const char *path,enum lx06_asr_profile profile) {
 if(profile!=LX06_ASR_CONTROL && profile!=LX06_ASR_QUESTION && profile!=LX06_ASR_WINDOW)return NULL;
 if(!path||!*path)path="/tmp/lx06-control-asr-probe.sock";
 if(strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return NULL;
 lx06_control_asr *a=calloc(1,sizeof(*a));if(!a)return NULL;a->fd=-1;a->profile=profile;
 a->stable_ms=1600;
 /* Only the account voice client opts into a shorter question endpoint. The
  * CONTROL profile used by Xiaozhi/Doubao retains its existing timing. Still
  * request and await a cloud FINAL; never promote a stable partial to text. */
 if(profile==LX06_ASR_QUESTION) {
  const char *value=getenv("LX06_QUESTION_STABLE_MS");
  if(value) {char *end;long ms=strtol(value,&end,10);if(*value&&!*end&&ms>=1000&&ms<=2000)a->stable_ms=(uint64_t)ms;}
 }
 /* The existing bridge already supports request-local VAD selection. Long
  * questions must not be finalized by cloud VAD at each short clause pause. */
 a->cloud_vad=profile==LX06_ASR_CONTROL;
 a->utterance_ms=profile==LX06_ASR_QUESTION?30000:4000;
 a->turn_ms=profile==LX06_ASR_QUESTION?45000:30000;
 /* Listen window: cloud VAD plus the existing 300 ms quiet tail. The tail is
  * the shipped QUESTION quiet gate, not a new threshold. §11-3 still wants a
  * device pass before anyone treats 300 ms as final. */
 if(profile==LX06_ASR_WINDOW) {
  a->cloud_vad=1; a->stable_ms=300;
  lx06_control_asr_window_timing(a,0,0);
 }
 int error=0;a->encoder=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&error);
 if(!a->encoder||error)goto bad;
 (void)opus_encoder_ctl(a->encoder,OPUS_SET_BITRATE(24000));
 (void)opus_encoder_ctl(a->encoder,OPUS_SET_COMPLEXITY(3));
 a->fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
 if(a->fd<0)goto bad;
 struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,path);
 if(connect(a->fd,(void*)&addr,sizeof(addr)))goto bad;
 if(profile==LX06_ASR_WINDOW) {window_readers++;a->window_registered=1;}
 return a;
bad:lx06_control_asr_close(a);return NULL;
}
int lx06_control_asr_feed(lx06_control_asr *a,int protect,const void *pcm,size_t n,uint64_t now) {
 if(!a||n%2||n>32000)return -1;
 if(!protect) {
  if(a->active && send_packet(a,"C",1))return -1;
  a->protect=a->active=a->tts_held=0;a->tts_hold_until=a->priority_until=0;
  a->used=a->pending_used=a->pending_head=0;a->pending_growth_failed=0;
  a->dialog[0]=0;a->ready_after=now+200;
  a->archive_used=a->replay_at=0;a->replay_pending=0;
  a->replay_first_partial=a->replay_reply=0;
  if(a->profile==LX06_ASR_WINDOW){a->speech_seen=0;a->speech_started=0;}
  a->last_pcm_at=0;a->quiet_ms=0;a->pcm_seen=0;return 0;
 }
 a->protect=1;
 if(a->tts_held) {
  if(now<a->tts_hold_until){defer_audio(a,pcm,n);return 0;}
  if(lx06_control_asr_tts_resume(a,now))return -1;
 }
 if(!a->active && now<a->ready_after){defer_audio(a,pcm,n);return 0;}
 if(!a->active && start(a,now))return -1;
 if(a->replay_pending) {
  int done=replay_flush(a);if(done<0)return -1;
  if(!done){defer_audio(a,pcm,n);return 0;}
 }
 if(a->ended) {
  // Preserve speech while the previous FINAL is in flight, including the
  // next turn's first syllable. QUESTION can retain four seconds for its
  // three-second final wait plus restart delay; CONTROL keeps two seconds.
  defer_audio(a,pcm,n);return 0;
 }
 if(a->pending_used) {
  size_t saved=a->pending_used,at=a->pending_head,capacity=pending_capacity(a);
  a->pending_used=a->pending_head=0;a->pending_growth_failed=0;
  while(saved) {
   size_t take;unsigned char *source=pending_span(a,at,&take);
   if(take>saved)take=saved;
   if(take>32000)take=32000;
   if(lx06_control_asr_feed(a,1,source,take,now))return -1;
   saved-=take;at=(at+take)%capacity;
  }
 }
 const unsigned char *p=pcm;
 while(n) {
  size_t take=sizeof(a->pcm)-a->used;if(take>n)take=n;
  memcpy(a->pcm+a->used,p,take);a->used+=take;p+=take;n-=take;
  if(a->used==sizeof(a->pcm)) {
   if(a->profile==LX06_ASR_QUESTION || a->profile==LX06_ASR_WINDOW) {
    if(a->profile==LX06_ASR_WINDOW && !a->speech_seen &&
       now-a->started<a->window_start_ms && !question_pcm_quiet(a->pcm,sizeof(a->pcm))) {
     a->speech_seen=1;a->speech_started=now;
    }
    question_note_pcm(a,a->pcm,sizeof(a->pcm),now);
   }
   unsigned char packet[4096];short samples[320];memcpy(samples,a->pcm,sizeof(samples));
   int bytes=opus_encode(a->encoder,samples,320,packet+3,sizeof(packet)-3);
   if(bytes<=0)return -1;
   packet[0]='A';packet[1]=(unsigned char)(bytes>>8);packet[2]=(unsigned char)bytes;
   if(send_packet(a,packet,(size_t)bytes+3))return -1;
   if(a->priority && a->archive_complete) {
    size_t size=(size_t)bytes+3;
    if(size>LX06_ASR_ARCHIVE_CAP-a->archive_used)a->archive_complete=0;
    else {memcpy(a->archive+a->archive_used,packet,size);a->archive_used+=size;}
   }
   a->used=0;
  }
 }
 return 0;
}
int lx06_control_asr_end_utterance(lx06_control_asr *a,uint64_t reply,uint64_t now) {
 if(!a||!a->active||a->ended||!a->first_partial||a->utterance_reply!=reply)return 0;
 if(send_packet(a,"E",1))return -1;
 a->ended=1;a->finish_at=now;
 trace("control-quiet-endpoint",now);
 return 1;
}
int lx06_control_asr_poll(lx06_control_asr *a,char *text,size_t cap,uint64_t now) {
 if(!a||!text||cap<2)return -1;
 *text=0;
 if(flush_outbox(a)<0)return -1;
 if(a->active && a->replay_pending && replay_flush(a)<0)return -1;
 if(now-a->last_ping>=5000) {if(send_packet(a,"P",1))return -1;a->last_ping=now;}
 for(int budget=0;budget<16;budget++) {
  ssize_t n=recv(a->fd,a->message,sizeof(a->message)-1,MSG_DONTWAIT|MSG_TRUNC);
  if(n<0) {if(errno==EAGAIN||errno==EWOULDBLOCK)break;if(errno==EINTR)continue;return -1;}
  if(!n||n>=(ssize_t)sizeof(a->message))return -1;
  a->message[n]=0;
  xz_json_doc *d=&a->doc;
  if(xz_json_parse(d,a->message,(size_t)n))return -1;
  char type[64];
  if(!xz_json_get_string(d,d->root,"type",type,sizeof(type))) {
   if(!strcmp(type,"error"))return -1;
   if(!strcmp(type,"ready")) {
    int epoch=0;
    if(!xz_json_get_int(d,d->root,"epoch",&epoch) && (uint32_t)epoch==a->epoch && a->active)
     if(xz_json_get_string(d,d->root,"dialog",a->dialog,sizeof(a->dialog)))return -1;
   }
   continue;
  }
  int h=xz_json_object_get(d,d->root,"header"),body=xz_json_object_get(d,d->root,"payload");
  char id[96],ns[64],name[64];
  if(!a->active||!a->protect||!*a->dialog||a->final_seen ||
     xz_json_get_string(d,h,"dialog_id",id,sizeof(id))||strcmp(id,a->dialog) ||
     xz_json_get_string(d,h,"namespace",ns,sizeof(ns))||xz_json_get_string(d,h,"name",name,sizeof(name)))continue;
  if(!strcmp(ns,"SpeechRecognizer")&&!strcmp(name,"StopCapture")) {
   if(!a->ended)trace("stop-capture",now);
   if(!a->ended && send_packet(a,"E",1))return -1;
   a->ended=1;if(!a->finish_at)a->finish_at=now;
  } else if(!strcmp(ns,"SpeechRecognizer")&&!strcmp(name,"RecognizeResult")) {
   int f=xz_json_object_get(d,body,"is_final"),results=xz_json_object_get(d,body,"results");
   if(f<0||d->tokens[f].end-d->tokens[f].start!=4||memcmp(a->message+d->tokens[f].start,"true",4)) {
    char hypothesis[2048];
    if(results>=0 && d->tokens[results].type==XZ_JSON_ARRAY &&
       !xz_json_get_string(d,d->tokens[results].first_child,"text",hypothesis,sizeof(hypothesis)) &&
       *hypothesis && (a->profile==LX06_ASR_QUESTION || strcmp(hypothesis,a->partial))) {
     if(strcmp(hypothesis,a->partial)){strcpy(a->partial,hypothesis);a->partial_changed=now;a->control_hint=0;}
     /* QUESTION refreshes the arrival clock for repeated partials. CONTROL
      * retains its established text-change-only timing. */
     a->last_partial=now;
     if(!a->first_partial){a->first_partial=a->replay_first_partial?a->replay_first_partial:now;a->utterance_reply=a->replay_first_partial?a->replay_reply:a->reply;}
     if(a->tts_held){a->tts_held=0;a->tts_hold_until=0;trace("tts-yield-late-partial",now);}
     trace("partial-updated",now);
    }
    continue;
   }
   a->result_reply=a->first_partial?a->utterance_reply:a->replay_first_partial?a->replay_reply:a->reply;
   if(getenv("LX06_CONTROL_ASR_TRACE")&&!strcmp(getenv("LX06_CONTROL_ASR_TRACE"),"1"))
    fprintf(stderr,"control-asr: final-buffer pending=%zu dropped=%zu at=%llu\n",a->pending_used,a->pending_dropped,(unsigned long long)now);
   a->final_seen=1;a->active=0;a->dialog[0]=0;a->ready_after=now+200;
   trace("final",now);
   if(results>=0 && d->tokens[results].type==XZ_JSON_ARRAY &&
      !xz_json_get_string(d,d->tokens[results].first_child,"text",text,cap) && *text)return 1;
   // Empty final replaces all hypotheses, never promotes a partial control word.
  }
 }
 if(a->tts_held && now>=a->tts_hold_until && lx06_control_asr_tts_resume(a,now))return -1;
 // Residual playback noise can prevent cloud VAD from ending the utterance.
 // Request a final after a stable hypothesis, but never act on the partial.
 int stable_endpoint=a->profile==LX06_ASR_WINDOW ? (a->speech_seen && question_quiet_ready(a,now))
  : (a->profile!=LX06_ASR_QUESTION || a->control_hint || question_quiet_ready(a,now));
 uint64_t stable_at=a->control_hint?a->partial_changed:a->last_partial;
 int endpoint=a->profile==LX06_ASR_WINDOW
  ? ((!a->speech_seen && now-a->started>=a->window_start_ms) ||
     (a->speech_seen && (now-a->speech_started>=a->window_max_ms ||
       (a->first_partial && stable_endpoint && now-stable_at>=a->stable_ms))))
  : (now-a->started>=a->turn_ms ||
    (a->first_partial && (now-a->first_partial>=a->utterance_ms ||
      (stable_endpoint && now-stable_at>=a->stable_ms))));
 if(a->active && !a->ended && endpoint) {
  trace("bounded-endpoint",now);
  if(send_packet(a,"E",1))return -1;
  a->ended=1;a->finish_at=now;
 }
 if(a->active && a->ended && now-a->finish_at>=3000) {
  trace("final-timeout",now);
  // ASR timeout resets only the auxiliary utterance, never the main session.
  if(send_packet(a,"C",1))return -1;
  a->active=0;a->dialog[0]=0;
 }
 return 0;
}
void lx06_control_asr_close(lx06_control_asr *a) {
 if(!a)return;
 if(a->fd>=0){close(a->fd);a->fd=-1;}
 if(a->encoder)opus_encoder_destroy(a->encoder);
 /* A failed open never acquired a reader and cannot retire another window.
  * Keep this instance counted until its actual socket/encoder are closed. */
 if(a->window_registered) {window_readers--;a->window_registered=0;}
 free(a->archive);
 free(a->outbox);
 free(a->question_pending);
 free(a);
}
