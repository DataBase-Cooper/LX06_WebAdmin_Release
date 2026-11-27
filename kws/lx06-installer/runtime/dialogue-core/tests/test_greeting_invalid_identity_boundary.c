/* Invalid recognition must not cancel an active greeting. Offline fixture. */
#include "../session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error assertions required
#endif
struct obs { unsigned stops, cancels, questions; };
static void stop(void *p,dc_tag t){(void)t;((struct obs *)p)->stops++;}
static void cancel(void *p,dc_tag t){(void)t;((struct obs *)p)->cancels++;}
static int question(void *p,dc_tag t,const char *s){(void)t;(void)s;((struct obs *)p)->questions++;return 0;}
static void close_remote(void *p){(void)p;}
static void light(void *p,dc_tag t,uint64_t seq,enum dc_state s){(void)p;(void)t;(void)seq;(void)s;}
int main(void){
 struct obs o={0};dc_session s;dc_config c={0};
 dc_session_ops ops={&o,stop,cancel,question,close_remote,light};
 assert(!dc_session_open(&s,1,&c,&ops,"wake",100));
 dc_tag tag=dc_session_greeting(&s,"hello",110);assert(tag.reply_generation);
 char too_long[DC_QUESTION_CAP+1];memset(too_long,'x',sizeof too_long-1);too_long[sizeof too_long-1]=0;
 int r=dc_session_text(&s,tag,too_long,1,120);
 if(r!=-1||o.stops||o.cancels||o.questions||!s.control.reply_live){
  fprintf(stderr,"FAIL: invalid final r=%d stop=%u cancel=%u question=%u live=%d\n",r,o.stops,o.cancels,o.questions,s.control.reply_live);return 1;
 }
 char malformed[]={ 'q', (char)0xff, 0 };
 r=dc_session_text(&s,tag,malformed,1,130);
 if(r!=-1||o.stops||o.cancels||o.questions||!s.control.reply_live){
  fprintf(stderr,"FAIL: malformed final r=%d stop=%u cancel=%u question=%u live=%d\n",r,o.stops,o.cancels,o.questions,s.control.reply_live);return 1;
 }
 r=dc_session_text(&s,tag,"...",1,140);
 if(r!=0||o.stops||o.cancels||o.questions||!s.control.reply_live){
  fprintf(stderr,"FAIL: punctuation final r=%d stop=%u cancel=%u question=%u live=%d\n",r,o.stops,o.cancels,o.questions,s.control.reply_live);return 1;
 }
 /* A finalized serial rebound to a new reply must not interrupt it. */
 struct obs o2={0};dc_session s2;dc_config c2={.barge_any=1};
 dc_session_ops ops2={&o2,stop,cancel,question,close_remote,light};
 assert(!dc_session_open(&s2,2,&c2,&ops2,"wake",200));
 assert(dc_session_text(&s2,(dc_tag){2,0,1},"question",1,210)==1);
 dc_tag late={2,s2.control.tag.reply_generation,1};
 r=dc_session_voice(&s2,late,1,0,220);
 if(r!=0||o2.stops||o2.cancels||!s2.control.reply_live){
  fprintf(stderr,"FAIL: finalized voice r=%d stop=%u cancel=%u live=%d\n",r,o2.stops,o2.cancels,s2.control.reply_live);return 1;
 }
 puts("greeting invalid recognition and finalized voice: PASS");return 0;
}
