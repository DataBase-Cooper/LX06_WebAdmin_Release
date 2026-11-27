#include "../audio_route.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned cases;
static dc_tag tag(unsigned generation,unsigned utterance){return (dc_tag){41,generation,utterance};}
static dc_audio_route *fresh(unsigned native){dc_audio_route *r=dc_audio_route_create(41,native);assert(r);assert(!dc_audio_route_ready(r));return r;}
static void frame(dc_audio_route *r,uint64_t utterance,int near,unsigned value){
 unsigned char pcm[320];memset(pcm,value,sizeof pcm);assert(!dc_audio_route_capture(r,pcm,sizeof pcm,utterance,near,100));
}
static dc_audio_action next(dc_audio_route *r,unsigned type){dc_audio_action a;assert(dc_audio_route_next(r,&a)==1);assert(a.type==type);return a;}
static void sent(dc_audio_route *r,dc_audio_action a){assert(!dc_audio_route_sent(r,&a,100));}
static void idle(dc_audio_route *r){dc_audio_action a;assert(!dc_audio_route_next(r,&a));}
static size_t upload_to_commit(dc_audio_route *r,dc_tag expected,unsigned char *out,size_t cap){
 size_t used=0;
 for(unsigned budget=0;budget<1000;budget++){
  dc_audio_action a;assert(dc_audio_route_next(r,&a)==1);
  if(a.type==DC_AUDIO_COMMIT){assert(dc_tag_equal(a.tag,expected));sent(r,a);return used;}
  assert(a.type==DC_AUDIO_MIC&&used+a.bytes<=cap);if(out)memcpy(out+used,a.pcm,a.bytes);used+=a.bytes;sent(r,a);
 }
 assert(0);return 0;
}
static void native_reply(dc_audio_route *r,uint64_t sequence,dc_tag expected){
 dc_tag got;assert(dc_audio_route_begin(r,sequence,100,&got)==DC_AUDIO_BEGIN_NEED_TAG);
 assert(!dc_audio_route_bind(r,sequence,expected,100));
 dc_audio_action a=next(r,DC_AUDIO_GRANT);assert(dc_tag_equal(a.tag,expected)&&a.sequence==sequence);sent(r,a);
 assert(dc_audio_route_accepts(r,sequence,expected.reply_generation));
}
static void final_watermark(void){
 dc_audio_route *r=fresh(0);frame(r,1,1,11);frame(r,1,1,12);
 assert(!dc_audio_route_question(r,tag(1,1),100));frame(r,2,0,99);
 unsigned char out[960];assert(upload_to_commit(r,tag(1,1),out,sizeof out)==640);assert(out[0]==11&&out[320]==12);
 dc_audio_status status;dc_audio_route_status(r,&status);assert(status.microphone_bytes==320);idle(r);
 dc_tag got;assert(dc_audio_route_begin(r,1,101,&got)==DC_AUDIO_BEGIN_KNOWN&&dc_tag_equal(got,tag(1,1)));
 sent(r,next(r,DC_AUDIO_GRANT));assert(dc_audio_route_accepts(r,1,1));
 assert(dc_audio_route_end(r,1,2)==0&&dc_audio_route_played(r,tag(1,1))==-EAGAIN);
 assert(dc_audio_route_end(r,1,1)==1&&dc_audio_route_played(r,tag(1,1))==1);
 dc_audio_route_destroy(r);cases++;
}
static void cancelled_before_begin(void){
 dc_audio_route *r=fresh(0);frame(r,1,1,11);assert(!dc_audio_route_question(r,tag(1,1),100));
 assert(upload_to_commit(r,tag(1,1),NULL,320)==320);
 assert(!dc_audio_route_cancel(r,tag(1,1),101));
 frame(r,2,1,22);assert(!dc_audio_route_question(r,tag(2,2),102));idle(r);
 dc_tag got;assert(dc_audio_route_begin(r,1,103,&got)==DC_AUDIO_BEGIN_CANCELLED&&dc_tag_equal(got,tag(1,1)));
 assert(!dc_audio_route_resumed(r,1,1));
 dc_audio_action a=next(r,DC_AUDIO_CANCEL);assert(a.sequence==1&&a.tag.reply_generation==1);sent(r,a);idle(r);
 assert(!dc_audio_route_cancel(r,tag(1,1),104));idle(r);
 assert(!dc_audio_route_resumed(r,1,2)&&!dc_audio_route_resumed(r,2,1));
 assert(dc_audio_route_resumed(r,1,1)==1);
 unsigned char out[320];assert(upload_to_commit(r,tag(2,2),out,sizeof out)==320&&out[0]==22);
 assert(dc_audio_route_begin(r,2,105,&got)==DC_AUDIO_BEGIN_KNOWN&&got.reply_generation==2);
 sent(r,next(r,DC_AUDIO_GRANT));assert(!dc_audio_route_accepts(r,1,1)&&dc_audio_route_accepts(r,2,2));
 dc_audio_route_destroy(r);cases++;
}
static void cancelled_before_commit(void){
 dc_audio_route *r=fresh(0);frame(r,1,1,11);assert(!dc_audio_route_question(r,tag(1,1),100));
 assert(!dc_audio_route_cancel(r,tag(1,1),101));frame(r,2,1,22);assert(!dc_audio_route_question(r,tag(2,2),102));
 unsigned char out[640];assert(upload_to_commit(r,tag(1,1),out,sizeof out)==320&&out[0]==11);idle(r);
 dc_tag got;assert(dc_audio_route_begin(r,1,103,&got)==DC_AUDIO_BEGIN_CANCELLED);
 sent(r,next(r,DC_AUDIO_CANCEL));assert(dc_audio_route_resumed(r,1,1)==1);
 assert(upload_to_commit(r,tag(2,2),out,sizeof out)==320&&out[0]==22);
 dc_audio_route_destroy(r);cases++;
}
static void strict_prefix_and_barrier(void){
 dc_audio_route *r=fresh(0);native_reply(r,1,tag(1,1));
 for(unsigned i=0;i<500;i++)frame(r,0,0,99);
 dc_audio_status status;dc_audio_route_status(r,&status);assert(status.microphone_bytes<=DC_AUDIO_PRE_BYTES);idle(r);
 for(unsigned i=0;i<20;i++)frame(r,2,1,22);
 assert(!dc_audio_route_cancel(r,tag(1,1),101));
 frame(r,2,1,23);assert(!dc_audio_route_question(r,tag(2,2),102));assert(!dc_audio_route_final(r,2));
 sent(r,next(r,DC_AUDIO_CANCEL));idle(r);
 assert(!dc_audio_route_end(r,1,1));assert(!dc_audio_route_resumed(r,999,1));idle(r);
 assert(dc_audio_route_resumed(r,1,1)==1);
 unsigned char out[20000];size_t n=upload_to_commit(r,tag(2,2),out,sizeof out);
 assert(n==DC_AUDIO_PRE_BYTES+21*320&&out[DC_AUDIO_PRE_BYTES]==22&&out[n-1]==23);
 dc_audio_route_destroy(r);cases++;
}
static void preserve_tail_prefix(void){
 dc_audio_route *r=fresh(0);native_reply(r,1,tag(1,1));
 for(unsigned i=0;i<40;i++)frame(r,0,0,99);
 frame(r,2,1,22);frame(r,2,1,23);
 assert(dc_audio_route_end(r,1,1)==1&&dc_audio_route_played(r,tag(1,1))==1);
 assert(!dc_audio_route_question(r,tag(2,2),100));
 unsigned char out[16000];size_t n=upload_to_commit(r,tag(2,2),out,sizeof out);
 assert(n==DC_AUDIO_PRE_BYTES+640&&out[n-640]==22&&out[n-1]==23);
 dc_audio_route_destroy(r);cases++;
}
static void native_and_stale(void){
 dc_audio_route *r=fresh(1);native_reply(r,1,tag(1,1));frame(r,2,1,22);
 dc_audio_action a=next(r,DC_AUDIO_MIC);assert(a.bytes==320);sent(r,a);
 dc_tag got;assert(dc_audio_route_begin(r,1,102,&got)==DC_AUDIO_BEGIN_STALE);
 assert(dc_audio_route_begin(r,2,102,&got)==-EPROTO);assert(dc_audio_route_accepts(r,1,1));
 dc_audio_route_destroy(r);cases++;
}
static void transaction(void){
 dc_audio_route *r=fresh(0);frame(r,1,1,11);dc_audio_action old=next(r,DC_AUDIO_MIC);
 frame(r,1,1,12);assert(dc_audio_route_sent(r,&old,100)==-ESTALE);
 dc_audio_action a=next(r,DC_AUDIO_MIC);assert(a.bytes==640&&a.pcm[0]==11&&a.pcm[320]==12);
 dc_audio_action b=next(r,DC_AUDIO_MIC);assert(dc_audio_route_sent(r,&a,100)==-ESTALE);sent(r,b);
 idle(r);assert(dc_audio_route_question(r,(dc_tag){42,1,1},100)==-EINVAL);
 dc_audio_route_destroy(r);cases++;
}
static void bounded_prefix(void){
 dc_audio_route *r=fresh(0);native_reply(r,1,tag(1,1));
 for(unsigned i=0;i<500;i++)frame(r,2,1,22);
 dc_audio_status status;dc_audio_route_status(r,&status);assert(status.microphone_bytes<=DC_AUDIO_MIC_CAP&&status.prefix_lost);
 assert(dc_audio_route_question(r,tag(2,2),100)==-ENOBUFS);
 assert(!dc_audio_route_final(r,2));dc_audio_route_status(r,&status);assert(!status.prefix_lost&&status.microphone_bytes<=DC_AUDIO_PRE_BYTES);
 frame(r,3,1,33);assert(!dc_audio_route_cancel(r,tag(1,1),101));assert(!dc_audio_route_question(r,tag(2,3),102));
 dc_audio_route_destroy(r);cases++;
}
static void capacity_and_timeout(void){
 dc_audio_route *r=fresh(0);
 for(unsigned i=0;i<DC_AUDIO_MIC_CAP/320;i++)frame(r,1,1,11);
 unsigned char sample[2]={0};assert(dc_audio_route_capture(r,sample,2,1,1,100)==-ENOBUFS);dc_audio_route_destroy(r);
 r=fresh(0);for(unsigned i=1;i<=DC_AUDIO_TURNS;i++)assert(!dc_audio_route_question(r,tag(i,i),100));
 assert(dc_audio_route_question(r,tag(5,5),100)==-ENOSPC);dc_audio_route_destroy(r);
 r=fresh(0);native_reply(r,1,tag(1,1));assert(!dc_audio_route_cancel(r,tag(1,1),100));
 assert(!dc_audio_route_tick(r,15100)&&dc_audio_route_tick(r,15101)==-ETIMEDOUT);dc_audio_route_destroy(r);cases++;
}
static void grant_identity(void){
 dc_audio_route *r=fresh(0);dc_tag got;assert(dc_audio_route_begin(r,1,100,&got)==DC_AUDIO_BEGIN_NEED_TAG);
 assert(dc_audio_route_bind(r,1,(dc_tag){42,1,1},100)==-EINVAL);
 assert(!dc_audio_route_accepts(r,1,1));assert(!dc_audio_route_bind(r,1,tag(1,1),100));
 dc_audio_action a=next(r,DC_AUDIO_GRANT);assert(!dc_audio_route_accepts(r,1,1));sent(r,a);
 assert(dc_audio_route_accepts(r,1,1));assert(!dc_audio_route_cancel(r,tag(1,1),100));assert(!dc_audio_route_accepts(r,1,1));
 dc_audio_route_destroy(r);cases++;
}
static void greeting_separate(void){
 for(unsigned native=0;native<2;native++){
  dc_audio_route *r=fresh(native);frame(r,2,1,22);
  assert(!dc_audio_route_greeting(r,tag(1,1),100));idle(r);
  assert(dc_audio_route_greeting(r,tag(1,1),100)==-EALREADY);
  dc_tag got;assert(dc_audio_route_begin(r,1,100,&got)==DC_AUDIO_BEGIN_KNOWN&&dc_tag_equal(got,tag(1,1)));
  sent(r,next(r,DC_AUDIO_GRANT));assert(dc_audio_route_end(r,1,1)==1&&dc_audio_route_played(r,tag(1,1))==1);
  assert(!dc_audio_route_question(r,tag(2,2),100));unsigned char out[640];assert(upload_to_commit(r,tag(2,2),out,sizeof out)==320&&out[0]==22);
  assert(dc_audio_route_begin(r,2,101,&got)==DC_AUDIO_BEGIN_KNOWN&&dc_tag_equal(got,tag(2,2)));
  dc_audio_route_destroy(r);
 }
 cases++;
}
static void greeting_cancel_before_begin(void){
 dc_audio_route *r=fresh(0);assert(!dc_audio_route_greeting(r,tag(1,1),100));
 frame(r,2,1,22);assert(!dc_audio_route_cancel(r,tag(1,1),101));assert(!dc_audio_route_question(r,tag(2,2),102));idle(r);
 dc_tag got;assert(dc_audio_route_begin(r,1,103,&got)==DC_AUDIO_BEGIN_CANCELLED&&dc_tag_equal(got,tag(1,1)));
 sent(r,next(r,DC_AUDIO_CANCEL));assert(dc_audio_route_resumed(r,1,1)==1);
 unsigned char out[640];assert(upload_to_commit(r,tag(2,2),out,sizeof out)==320&&out[0]==22);
 assert(dc_audio_route_begin(r,2,104,&got)==DC_AUDIO_BEGIN_KNOWN&&dc_tag_equal(got,tag(2,2)));
 dc_audio_route_destroy(r);cases++;
}
int main(void){
 final_watermark();cancelled_before_begin();cancelled_before_commit();strict_prefix_and_barrier();preserve_tail_prefix();
 native_and_stale();transaction();bounded_prefix();capacity_and_timeout();grant_identity();greeting_separate();greeting_cancel_before_begin();
 printf("audio-route cases=%u passed\n",cases);return 0;
}
