/* Offline observation, not acoustic ASR: real adapter + synthetic PCM and peer.
 * The five one-second spans stand for labeled parts of a hypothetical utterance;
 * their numeric PCM markers are not recorded speech and cannot be transcribed.
 * No poll is run during the five-second stall; normal polling has a 3 s timeout.
 */
#include "../asr/control_asr.c"
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static void send_msg(int fd,const char *s) { assert(send(fd,s,strlen(s),0)==(ssize_t)strlen(s)); }
static unsigned drain(int fd) {
 unsigned audio=0; char buf[8192]; ssize_t n;
 while((n=recv(fd,buf,sizeof buf,MSG_DONTWAIT))>0) if(buf[0]=='A') audio++;
 assert(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)); return audio;
}
static void run(int profile) {
 int fd[2],err=0,rc; char text[2048]; int16_t pcm[320];
 assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,fd));
 int sz=1048576; assert(!setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&sz,sizeof sz));
 lx06_control_asr *a=calloc(1,sizeof *a); assert(a);a->fd=fd[0];a->profile=profile;
 a->stable_ms=1600;a->turn_ms=45000;a->utterance_ms=30000;
 a->encoder=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&err);assert(a->encoder&&!err);
 memset(pcm,0,sizeof pcm);assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,1000));assert(drain(fd[1])==1);
 send_msg(fd[1],"{\"type\":\"ready\",\"dialog\":\"old\",\"epoch\":1}");
 send_msg(fd[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"StopCapture\",\"dialog_id\":\"old\"},\"payload\":{}}");
 assert(!lx06_control_asr_poll(a,text,sizeof text,1100));assert(a->ended);assert(!drain(fd[1]));
 /* 50 x 20ms frames per span: synthetic question-prefix, interruption,
    question-middle, goodbye, question-suffix. */
 for(int frame=0;frame<250;frame++) {
  for(int j=0;j<320;j++) pcm[j]=(int16_t)(1000*(1+frame/50));
  rc=lx06_control_asr_feed(a,1,pcm,sizeof pcm,1110+20*(uint64_t)frame);assert(rc==0);
 }
 size_t expected=profile==LX06_ASR_QUESTION?128000:64000;
 assert(a->pending_used==expected && a->pending_dropped==160000-expected);
 assert(!drain(fd[1])); /* no overflow message on the adapter socket */
 int first=profile==LX06_ASR_QUESTION?1:3;
 for(size_t k=0;k<expected/2;k++) {
  size_t pos=(a->pending_head+k*2)%pending_capacity(a),avail;unsigned char *p=pending_span(a,pos,&avail);
  assert(avail>=2);int value=(int)(p[0]|((unsigned)p[1]<<8));
  assert(value==1000*(1+first+(int)(k/16000)));
 }
 send_msg(fd[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\"old\"},\"payload\":{\"is_final\":true,\"results\":[{\"text\":\"\"}]}}");
 rc=lx06_control_asr_poll(a,text,sizeof text,6200);assert(rc==0 && !*text && !a->active);
 assert(!lx06_control_asr_feed(a,1,NULL,0,6400));
 unsigned packets=drain(fd[1]);assert(packets==expected/640);assert(!a->pending_used);
 printf("profile=%s feed_errors=0 dropped=%zu retained=%zu first_span=%d last_span=4 final_rc=%d replay_audio_packets=%u\n",profile==LX06_ASR_QUESTION?"QUESTION":"CONTROL",a->pending_dropped,expected,first,rc,packets);
 lx06_control_asr_close(a);close(fd[1]);
}
static void normal_poll(int profile) {
 int fd[2],err=0;char text[2048];int16_t pcm[320]={0};
 assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,fd));
 lx06_control_asr *a=calloc(1,sizeof *a);assert(a);a->fd=fd[0];a->profile=profile;
 a->stable_ms=1600;a->turn_ms=45000;a->utterance_ms=30000;
 a->encoder=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&err);assert(a->encoder&&!err);
 assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,1000));drain(fd[1]);
 send_msg(fd[1],"{\"type\":\"ready\",\"dialog\":\"old\",\"epoch\":1}");
 send_msg(fd[1],"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"StopCapture\",\"dialog_id\":\"old\"},\"payload\":{}}");
 assert(!lx06_control_asr_poll(a,text,sizeof text,1100));drain(fd[1]);
 int frames=0;
 while(a->active && frames<250) {
  uint64_t now=1110+20*(uint64_t)frames;
  assert(!lx06_control_asr_feed(a,1,pcm,sizeof pcm,now));frames++;
  assert(!lx06_control_asr_poll(a,text,sizeof text,now));drain(fd[1]);
 }
 assert(frames==151 && !a->active);
 assert(a->pending_dropped==(profile==LX06_ASR_CONTROL?32640U:0U));
 printf("polling profile=%s frames=%d timeout_pending=%zu dropped=%zu feed_and_poll_errors=0\n",profile==LX06_ASR_QUESTION?"QUESTION":"CONTROL",frames,a->pending_used,a->pending_dropped);
 lx06_control_asr_close(a);close(fd[1]);
}
int main(void) { run(LX06_ASR_CONTROL);run(LX06_ASR_QUESTION);normal_poll(LX06_ASR_CONTROL);normal_poll(LX06_ASR_QUESTION);return 0; }
