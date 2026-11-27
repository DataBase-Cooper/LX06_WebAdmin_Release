#define _POSIX_C_SOURCE 200809L
#include "../session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error assertions required
#endif
struct observed {char order[256],question[2048];unsigned used,requests,ends,stops;dc_tag tag;enum dc_state state;uint64_t seq;};
static void append(struct observed *o,char c){assert(o->used+1<sizeof o->order);o->order[o->used++]=c;o->order[o->used]=0;}
static void stop(void *ctx,dc_tag tag){struct observed *o=ctx;o->stops++;o->tag=tag;append(o,'S');}
static void cancel(void *ctx,dc_tag tag){struct observed *o=ctx;assert(dc_tag_equal(o->tag,tag));append(o,'C');}
static int question(void *ctx,dc_tag tag,const char *text){struct observed *o=ctx;o->requests++;o->tag=tag;strcpy(o->question,text);append(o,'Q');return 0;}
static void close_remote(void *ctx){append(ctx,'X');}
static void light(void *ctx,dc_tag tag,uint64_t seq,enum dc_state state){struct observed *o=ctx;assert(seq>o->seq);assert(tag.session_epoch);o->seq=seq;o->state=state;if(state==DC_CLOSED)o->ends++;append(o,'L');}
static unsigned checks;
#define CHECK(x) do{assert(x);checks++;}while(0)
int main(void){
 dc_session s;struct observed o={0};dc_config config={.idle_ms=10000,.tail_ms=300};
 strcpy(config.interrupt,"等一下,你说得不对,等等");strcpy(config.goodbye,"结束对话,拜拜了哦");
 dc_session_ops ops={&o,stop,cancel,question,close_remote,light};
 CHECK(dc_session_open(&s,123,&config,&ops,"召唤马斯克",100)==0);
 CHECK(o.state==DC_CONNECTING&&o.ends==0);
 CHECK(dc_session_ready(&s,110)==0&&o.state==DC_LISTENING);
 dc_tag greeting=dc_session_greeting(&s,"我在呢，请说。",120);
 CHECK(greeting.reply_generation==1&&o.state==DC_THINKING);
 dc_session_spoken(&s,greeting,"我在呢，请说。",130);
 CHECK(dc_session_speaking(&s,greeting,140)==0&&o.state==DC_SPEAKING);
 dc_tag source={123,1,1};
 CHECK(dc_session_text(&s,source,"请用两句话",0,150)==0&&o.requests==0);
 o.used=0;
 CHECK(dc_session_text(&s,source,"请用两句话介绍北京故宫",1,200)==1);
 CHECK(o.stops==1&&o.requests==1&&!strcmp(o.question,"请用两句话介绍北京故宫"));
 CHECK(!strncmp(o.order,"SCLQ",4)&&s.control.tag.reply_generation==2);
 dc_tag reply=s.control.tag;
 CHECK(dc_session_speaking(&s,reply,210)==0&&o.state==DC_SPEAKING);
 CHECK(dc_session_waiting(&s,reply,220)==0&&o.state==DC_THINKING&&o.ends==0);
 CHECK(dc_session_speaking(&s,reply,230)==0&&o.ends==0);
 source=(dc_tag){123,2,2};o.used=0;
 CHECK(dc_session_text(&s,source,"等一下",0,240)==1);
 CHECK(!strncmp(o.order,"SCL",3)&&o.state==DC_LISTENING&&o.ends==0);
 CHECK(dc_session_text(&s,source,"等等，等一下，你说得不对",0,250)==0&&o.stops==2);
 CHECK(dc_session_text(&s,source,"等等，你说得不对。一加一等于几",1,260)==1);
 CHECK(o.requests==2&&!strcmp(o.question,"一加一等于几"));
 /* Late recognition for a finalized serial is stale even if its generation
  * was rebound to the new reply. It must not execute a goodbye control. */
 dc_tag replay={123,s.control.tag.reply_generation,2};
 CHECK(dc_session_text(&s,replay,"结束对话",0,265)==0);
 CHECK(!s.control.closed);
 CHECK(dc_session_text(&s,source,"等一下，旧结果",1,270)==0&&o.requests==2);
 CHECK(dc_session_speaking(&s,reply,280)==-1&&o.ends==0);
 /* An obsolete completion cannot shorten the current reply's echo guard. */
 dc_session_spoken(&s,s.control.tag,"active reply",275);
 uint64_t echo_deadline=s.echo_until_ms;
 CHECK(dc_session_reply_done(&s,reply,280)==-1);
 CHECK(s.echo_until_ms==echo_deadline);
 reply=s.control.tag;
 CHECK(dc_session_reply_done(&s,reply,290)==0&&o.state==DC_LISTENING&&o.ends==0);
 CHECK(dc_session_tick(&s,500)==0&&o.ends==0);
 source=(dc_tag){123,reply.reply_generation,3};o.used=0;
 CHECK(dc_session_text(&s,source,"拜拜了哦，结束对话",0,600)==1);
 CHECK(s.control.state==DC_CLOSING&&o.ends==0&&!strncmp(o.order,"SXL",3));
 CHECK(dc_session_close(&s,610)==0&&o.ends==0);
 CHECK(dc_session_finish_close(&s,620)==1&&o.ends==1&&o.state==DC_CLOSED);
 CHECK(dc_session_finish_close(&s,630)==0&&o.ends==1);
 CHECK(dc_session_reply_done(&s,reply,640)==-1&&o.ends==1);
 /* A final goodbye overlapping a greeting closes in one stop/cancel pass. */
 dc_session g;struct observed go={0};dc_session_ops gops={&go,stop,cancel,question,close_remote,light};
 CHECK(dc_session_open(&g,124,&config,&gops,NULL,100)==0);
 dc_tag gt=dc_session_greeting(&g,"我在呢，请说。",120);
 CHECK(gt.session_epoch==124);go.used=0;go.order[0]=0;
 CHECK(dc_session_text(&g,(dc_tag){124,gt.reply_generation,1},"等一下，结束对话",1,150)==1);
 CHECK(go.stops==1&&!strcmp(go.order,"SCXL")&&go.requests==0&&g.control.closed);
 CHECK(dc_session_text(&g,(dc_tag){124,gt.reply_generation,1},"结束对话",1,151)==0);
 CHECK(go.stops==1);
 printf("{\"passed\":true,\"checks\":%u,\"device_access\":false,\"light_end_only_after_cleanup\":true}\n",checks);
 return 0;
}
