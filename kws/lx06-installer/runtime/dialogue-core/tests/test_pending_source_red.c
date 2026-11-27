/* Offline counterexample: actual adapter, recognition, session and seqpacket.
 * The peer supplies deterministic synthetic ASR results; no cloud recognition. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include "../asr/control_asr.c"
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static unsigned stops,cancels,questions;
static void stop(void *p,dc_tag t){(void)p;(void)t;stops++;}
static void cancel(void *p,dc_tag t){(void)p;(void)t;cancels++;}
static int fixture_question(void *p,dc_tag t,const char *q){(void)p;(void)t;(void)q;questions++;return 0;}
static void close_remote_test(void *p){(void)p;}
static void fixture_light(void *p,dc_tag t,uint64_t n,enum dc_state s){(void)p;(void)t;(void)n;(void)s;}
static void message(int fd,const char *s){ssize_t n=send(fd,s,strlen(s),0);assert(n==(ssize_t)strlen(s));}
static unsigned drain(int fd){char b[8192];ssize_t n;unsigned audio=0;while((n=recv(fd,b,sizeof b,MSG_DONTWAIT))>0){if(b[0]=='A')audio++;}assert(errno==EAGAIN||errno==EWOULDBLOCK);return audio;}
int main(void){
 int fds[2],error=0;assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,fds));
 struct lx06_control_asr *a=calloc(1,sizeof *a);assert(a);a->fd=fds[0];a->profile=LX06_ASR_QUESTION;a->stable_ms=1600;a->turn_ms=45000;a->utterance_ms=30000;
 a->encoder=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&error);assert(a->encoder&&!error);
 struct core c={0};c.asr=a;dc_config cfg={0};strcpy(cfg.interrupt,"等一下");
 dc_session_ops ops={NULL,stop,cancel,fixture_question,close_remote_test,fixture_light};
 assert(!dc_session_open(&c.session,11,&cfg,&ops,"",100));assert(!dc_session_ready(&c.session,101));
 dc_tag old=dc_session_reply(&c.session,1,102);assert(old.reply_generation==1);
 lx06_control_asr_reply(a,old.reply_generation);int16_t pcm[320];for(int i=0;i<320;i++)pcm[i]=(int16_t)(i*27-4000);
 assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,1000));drain(fds[1]);
 message(fds[1],"{\"type\":\"ready\",\"dialog\":\"old\",\"epoch\":1}");
 message(fds[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"StopCapture\",\"dialog_id\":\"old\"},\"payload\":{}}");
 char out[2048];assert(!lx06_control_asr_poll(a,out,sizeof out,1100));drain(fds[1]);
 /* These bytes arrive while reply 1 owns playback, during FINAL wait. */
 assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,1110));lx06_control_asr_bind_source(a);assert(a->pending_used==sizeof pcm);
 message(fds[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\"old\"},\"payload\":{\"is_final\":true,\"results\":[{\"text\":\"\"}]}}");
 assert(!lx06_control_asr_poll(a,out,sizeof out,1120));
 assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,1200));assert(a->pending_used==2*sizeof pcm);
 /* Independently cancel old and start a new answer during ready_after. */
 assert(dc_session_text(&c.session,(dc_tag){11,1,1},"等一下",0,1210)==1);assert(cancels==1);
 dc_tag next=dc_session_reply(&c.session,2,1220);assert(next.reply_generation==2);
 lx06_control_asr_reply(a,2);assert(!lx06_control_asr_feed(a,1,NULL,0,1320));
 assert(a->active&&a->turn_serial==2&&!a->pending_used);assert(drain(fds[1])==2);
 message(fds[1],"{\"type\":\"ready\",\"dialog\":\"next\",\"epoch\":2}");
 message(fds[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\"next\"},\"payload\":{\"is_final\":false,\"results\":[{\"text\":\"等一下\"}]}}");
 recognition(&c,1330);
 fprintf(stderr,"pending-old-reply=1, recognized-reply=%llu, serial=%llu, cancels=%u, current-live=%d\n",(unsigned long long)a->utterance_reply,(unsigned long long)a->turn_serial,cancels,c.session.control.reply_live);
 int ok=cancels==1&&c.session.control.reply_live;
 lx06_control_asr_close(a);close(fds[1]);
 if(!ok){fprintf(stderr,"RED: old pending audio can cancel new reply through real recognition/session\n");return 1;}
 puts("PASS: old pending audio did not cancel new reply");return 0;
}
