/* Offline production reactor/session/control configuration boundary. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static unsigned stops,cancels,questions;
static char result[DC_QUESTION_CAP];
static void st(void *p,dc_tag t){stops++;stop_local(p,t);}
static void ca(void *p,dc_tag t){(void)p;(void)t;cancels++;}
static int qu(void *p,dc_tag t,const char *q){(void)p;(void)t;questions++;strcpy(result,q);return 0;}
static void cl(void *p){(void)p;}
static void li(void *p,dc_tag t,uint64_t n,enum dc_state s){(void)p;(void)t;(void)n;(void)s;}
static dc_session_ops ops(struct core *c){dc_session_ops o={c,st,ca,qu,cl,li};return o;}
static void token(char *p,size_t n){memset(p,'a',n);p[n]=0;}
static void boundary(void){
 struct core c={0};dc_config cfg={0};dc_session_ops o=ops(&c);
 token(cfg.interrupt,127);assert(!dc_session_open(&c.session,1,&cfg,&o,"",1));
 token(cfg.interrupt,128);assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 strcpy(cfg.interrupt,"等等,");token(cfg.interrupt+strlen(cfg.interrupt),127);assert(!dc_session_open(&c.session,1,&cfg,&o,"",1));
 strcpy(cfg.interrupt,"等等,");token(cfg.interrupt+strlen(cfg.interrupt),128);assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 cfg.interrupt[0]=0;for(int i=0;i<42;i++)strcat(cfg.interrupt,"等");strcat(cfg.interrupt,"a");assert(strlen(cfg.interrupt)==127);assert(!dc_session_open(&c.session,1,&cfg,&o,"",1));
 strcat(cfg.interrupt,"b");assert(strlen(cfg.interrupt)==128);assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 strcpy(cfg.interrupt,"等等，停一下、好吧");token(cfg.goodbye,127);assert(!dc_session_open(&c.session,1,&cfg,&o,"",1));
 token(cfg.goodbye,128);assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 strcpy(cfg.goodbye,"结束对话");strcpy(cfg.interrupt,"\xff");assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 strcpy(cfg.interrupt,"等等");strcpy(cfg.goodbye,"\xc3");assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
 memset(cfg.interrupt,'a',sizeof cfg.interrupt);assert(dc_session_open(&c.session,1,&cfg,&o,"",1)==-1);
}
static void scenario(int greeting,int bad_config,int bad_text,int mode){
 struct core c={0};dc_config cfg={0};dc_session_ops o=ops(&c);stops=cancels=questions=0;result[0]=0;
 strcpy(cfg.interrupt,"等等,停一下");cfg.barge_any=mode;assert(!dc_session_open(&c.session,1,&cfg,&o,"",1));
 dc_tag t=greeting?dc_session_greeting(&c.session,"你好",2):dc_session_reply(&c.session,1,2);assert(t.session_epoch);
 if(bad_config)token(c.session.control.config.interrupt,128); /* invariant corruption after admission */
 int r=dc_session_text(&c.session,(dc_tag){1,t.reply_generation,2},bad_text?"等等\xff":"等等，完整问题",1,3);
 if(bad_config||bad_text){assert(r==-1);assert(!stops&&!cancels&&!questions&&c.session.control.reply_live&&!c.session.control.last_final_utterance);}
 else {assert(r==1);assert(stops==1&&cancels==1&&questions==1&&!strcmp(result,"完整问题"));}
}
int main(void){boundary();for(int g=0;g<2;g++)for(int m=0;m<2;m++){scenario(g,0,0,m);scenario(g,0,1,m);scenario(g,1,0,m);}puts("configuration and final boundary: PASS");return 0;}
