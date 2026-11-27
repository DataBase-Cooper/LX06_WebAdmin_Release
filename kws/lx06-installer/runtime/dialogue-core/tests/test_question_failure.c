/* Production recognition -> session -> reactor question -> audio route.
 * ASR is synthetic; no transport, device, network or backend is started. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <assert.h>
#ifdef NDEBUG
#error assertions required
#endif
static int polled, closed_asr;
int lx06_control_asr_poll(lx06_control_asr *a,char *text,size_t cap,uint64_t now){(void)a;(void)now;assert(cap>6);if(polled++)return 0;memcpy(text,"hello",6);return 1;}
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *a){(void)a;return 0;}
uint64_t lx06_control_asr_result_serial(const lx06_control_asr *a){(void)a;return 1;}
const char *lx06_control_asr_partial(const lx06_control_asr *a,uint64_t *reply){(void)a;*reply=0;return "";}
uint64_t lx06_control_asr_partial_serial(const lx06_control_asr *a){(void)a;return 0;}
void lx06_control_asr_close(lx06_control_asr *a){(void)a;closed_asr++;}
void dc_io_capture_close(dc_io_capture *c){(void)c;assert(0);}
static void no_light(void *p,dc_tag t,uint64_t seq,enum dc_state s){(void)p;(void)t;(void)seq;(void)s;}
int main(void){
 struct core c={0};dc_config cfg={0};dc_session_ops ops={&c,stop_local,cancel_remote,question,close_remote,no_light};
 c.audio=1;c.route=dc_audio_route_create(17,0);assert(c.route);assert(!dc_audio_route_ready(c.route));
 for(unsigned i=1;i<=DC_AUDIO_TURNS;i++){dc_tag t={17,i,i+10};int r=dc_audio_route_question(c.route,t,1);assert(!r);}
 int r=dc_session_open(&c.session,17,&cfg,&ops,"",1);assert(!r);r=dc_session_ready(&c.session,2);assert(!r);
 c.asr=(lx06_control_asr *)&c;
 recognition(&c,3);
 assert(polled==1&&closed_asr==1&&c.closing&&c.session.control.closed);
 if(!c.failed){fprintf(stderr,"FAIL: failed route question closed session but reactor reports success\n");return 1;}
 dc_audio_route_destroy(c.route);
 /* A real account adapter preflight failure also must not become success. */
 struct core account={0};dc_session_ops account_ops={&account,stop_local,cancel_remote,question,close_remote,no_light};
 r=dc_session_open(&account.session,18,&cfg,&account_ops,"",1);assert(!r);r=dc_session_ready(&account.session,2);assert(!r);
 account.asr=(lx06_control_asr *)&account;polled=0;closed_asr=0;
 recognition(&account,3);
 assert(polled==1&&closed_asr==1&&account.closing&&account.session.control.closed);
 if(!account.failed){fprintf(stderr,"FAIL: failed account question reported success\n");return 1;}
 puts("question failure propagated: PASS (route and account preflight)");return 0;
}
