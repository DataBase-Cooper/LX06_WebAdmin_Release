/* Production recognition and cue state, with synthetic ASR boundary. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static const char *input; static int final_event; static uint64_t generation,serial;
int lx06_control_asr_poll(lx06_control_asr *a,char *text,size_t cap,uint64_t now){(void)a;(void)now;if(!final_event)return 0;size_t n=strlen(input)+1;assert(n<=cap);memcpy(text,input,n);return 1;}
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *a){(void)a;return generation;}
uint64_t lx06_control_asr_result_serial(const lx06_control_asr *a){(void)a;return serial;}
const char *lx06_control_asr_partial(const lx06_control_asr *a,uint64_t *reply){(void)a;*reply=generation;return final_event?"":input;}
uint64_t lx06_control_asr_partial_serial(const lx06_control_asr *a){(void)a;return serial;}
static void noop_stop(void *p,dc_tag t){(void)p;(void)t;}
static unsigned questions;
static int noop_question(void *p,dc_tag t,const char *s){(void)p;(void)t;(void)s;questions++;return 0;}
static void noop_close(void *p){(void)p;}
static void noop_light(void *p,dc_tag t,uint64_t q,enum dc_state s){(void)p;(void)t;(void)q;(void)s;}
int main(int argc,char **argv){
 assert(argc==2);int scenario=atoi(argv[1]);struct core c={0};dc_config cfg={0};strcpy(cfg.interrupt,"等一下");
 dc_session_ops ops={&c,noop_stop,noop_stop,noop_question,noop_close,noop_light};
 assert(!dc_session_open(&c.session,9,&cfg,&ops,NULL,100));
 assert(!dc_media_cue_begin(&c.cue,9,NULL));c.phone=1;c.asr=(lx06_control_asr *)&c;
 input=scenario>=4&&scenario<=5?"等一下\xff":"等一下，完整问题";
 final_event=scenario%2;generation=scenario<2?1:0;serial=2;
 if(scenario<2)c.session.control.tag.reply_generation=2; /* old generation 1 */
 if(scenario>=2&&scenario<=3)c.session.control.last_final_utterance=2;
 recognition(&c,101);
 int should_stop=scenario>=6;
 if(dc_media_cue_active(&c.cue)==should_stop){fprintf(stderr,"FAIL scenario %d cue active=%d expected stop=%d\n",scenario,dc_media_cue_active(&c.cue),should_stop);return 1;}
 if(c.session.control.last_final_utterance!=(scenario>=2&&scenario<=3?2u:0u)){fprintf(stderr,"FAIL partial/final semantics scenario %d\n",scenario);return 1;}
 if(questions||c.failed!=(scenario==5)){fprintf(stderr,"FAIL question/failure behavior scenario %d\n",scenario);return 1;}
 printf("scenario %d PASS\n",scenario);return 0;
}
