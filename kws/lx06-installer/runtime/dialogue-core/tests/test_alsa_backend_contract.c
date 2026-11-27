/* Offline: real ALSA wrapper and player; scripted ABI boundary, no devices.
 * Build: gcc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread
 *   tests/test_alsa_backend_contract.c io/player.c -ldl -o <scratch>/fixture */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include "../io/alsa.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
struct _snd_pcm { int unused; };
struct _snd_pcm_sw_params { int unused; };
static struct _snd_pcm fake_pcm;
static struct _snd_pcm_sw_params fake_sw;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static long script[16]; static size_t count, cursor; static int prepares, prep_result, closes;
static int16_t observed[320]; static size_t observed_n;
static int f_open(pcm_t **p,const char *name,int stream,int mode) { (void)name; CHECK(stream==0 && mode==1); *p=&fake_pcm; return 0; }
static int f_close(pcm_t *p) { CHECK(p==&fake_pcm); pthread_mutex_lock(&mu); closes++; pthread_mutex_unlock(&mu); return 0; }
static int f_params(pcm_t *p,int f,int a,unsigned c,unsigned r,int res,unsigned us) { CHECK(p==&fake_pcm && f==2 && a==3 && c==1 && r==16000 && res==1 && us==80000);return 0; }
static int f_malloc(sw_t **s) { *s=&fake_sw; return 0; }
static void f_free(sw_t *s) { CHECK(s==&fake_sw); }
static int f_current(pcm_t *p,sw_t *s) { CHECK(p==&fake_pcm && s==&fake_sw); return 0; }
static int f_start(pcm_t *p,sw_t *s,frames_t v) { CHECK(p==&fake_pcm && s==&fake_sw && v==1); return 0; }
static int f_avail(pcm_t *p,sw_t *s,frames_t v) { CHECK(p==&fake_pcm && s==&fake_sw && v==160); return 0; }
static int f_count(pcm_t *p) { CHECK(p==&fake_pcm); return 0; }
static int f_descriptors(pcm_t *p,struct pollfd *f,unsigned n) { (void)f; CHECK(p==&fake_pcm && n==0);return 0; }
static long f_write(pcm_t *p,const void *data,frames_t n) {
    CHECK(p==&fake_pcm); pthread_mutex_lock(&mu);
    long r=cursor<count?script[cursor++]:-EAGAIN;
    if(r>0) { CHECK((frames_t)r<=n && observed_n+(size_t)r<=320); memcpy(observed+observed_n,data,(size_t)r*2);observed_n+=(size_t)r; }
    pthread_mutex_unlock(&mu); return r;
}
static int f_delay(pcm_t *p,long *frames) { CHECK(p==&fake_pcm); *frames=0;return 0; }
static int f_prepare(pcm_t *p) { CHECK(p==&fake_pcm);pthread_mutex_lock(&mu);prepares++;int r=prep_result;pthread_mutex_unlock(&mu);return r; }
static int f_drop(pcm_t *p) { CHECK(p==&fake_pcm);return 0; }
static dc_io_alsa a;
static dc_io_playback_backend backend(void) {
    memset(&a,0,sizeof a); strcpy(a.device,"offline");
    a.pcm_open=f_open; a.pcm_close=f_close; a.set_params=f_params;a.sw_malloc=f_malloc;a.sw_free=f_free;
    a.sw_current=f_current;a.sw_start=f_start;a.sw_avail=f_avail;a.sw_apply=f_current;
    a.poll_count=f_count;a.poll_descriptors=f_descriptors;a.writei=f_write;a.pcm_delay=f_delay;
    a.pcm_prepare=f_prepare;a.pcm_drop=f_drop;
    return (dc_io_playback_backend){&a,alsa_open,alsa_poll,alsa_write,alsa_delay,alsa_prepare,alsa_drop,alsa_close};
}
struct ref { size_t n,frames[8]; uint64_t op[8],offset[8]; int16_t first[8]; };
static void accepted(void *c,dc_tag tag,const int16_t *pcm,size_t n,unsigned rate,uint64_t op,uint64_t off) {
    (void)tag;CHECK(rate==16000);struct ref *r=c;pthread_mutex_lock(&mu);CHECK(r->n<8);size_t i=r->n++;r->frames[i]=n;r->op[i]=op;r->offset[i]=off;r->first[i]=*pcm;pthread_mutex_unlock(&mu);
}
static void wait_refs(struct ref *r,size_t n) { for(int i=0;i<300;i++){pthread_mutex_lock(&mu);size_t v=r->n;pthread_mutex_unlock(&mu);if(v>=n)return;struct timespec t={0,10000000};nanosleep(&t,NULL);}CHECK(0 && "reference timeout"); }
static void reset(const long *s,size_t n,int result) { pthread_mutex_lock(&mu); memcpy(script,s,n*sizeof *s);count=n;cursor=observed_n=0;prepares=closes=0;prep_result=result;pthread_mutex_unlock(&mu); }
static void run_case(const long *s,size_t n,int recover) {
    reset(s,n,0);dc_io_playback_backend b=backend();struct ref r={0};dc_io_reference ref={&r,NULL,accepted};
    dc_io_player *p=dc_io_player_create_with_reference(&b,&ref);CHECK(p);dc_tag tag={.session_epoch=1,.reply_generation=1};int16_t pcm[160];for(int i=0;i<160;i++)pcm[i]=(int16_t)(i+1);
    CHECK(dc_io_player_begin(p,tag,16000)==0);CHECK(dc_io_player_write(p,tag,pcm,sizeof pcm)==0);wait_refs(&r,2);
    pthread_mutex_lock(&mu);CHECK(r.n==2 && r.frames[0]==1 && r.frames[1]==159 && r.offset[0]==0 && r.offset[1]==1 && r.first[0]==1 && r.first[1]==2);
    CHECK((r.op[0]!=r.op[1])==recover);CHECK(prepares==recover);CHECK(observed_n==160 && memcmp(observed,pcm,sizeof pcm)==0);pthread_mutex_unlock(&mu);
    dc_io_player_destroy(p);printf("PASS %s: 1+159 exact; operations %s; prepares=%d\n",recover?"recovered underrun":"retry fragmentation",recover?"fenced":"same",recover);
}
static void failure_case(void) {
    const long s[]={-EPIPE};reset(s,1,-EIO);dc_io_playback_backend b=backend();dc_io_player *p=dc_io_player_create(&b);CHECK(p);dc_tag tag={.session_epoch=2,.reply_generation=1};int16_t pcm[160]={0};
    CHECK(dc_io_player_begin(p,tag,16000)==0);CHECK(dc_io_player_write(p,tag,pcm,sizeof pcm)==0);
    int found=0;for(int i=0;i<300&&!found;i++){dc_io_event e;while(dc_io_player_next_event(p,&e)==1)if(e.type==DC_IO_ERROR){CHECK(e.error==EIO);found=1;}if(!found){struct timespec t={0,10000000};nanosleep(&t,NULL);}}
    CHECK(found);pthread_mutex_lock(&mu);CHECK(prepares==1 && closes==1 && observed_n==0);pthread_mutex_unlock(&mu);dc_io_player_destroy(p);puts("PASS prepare failure: EIO event, closed, no accepted PCM");
}
static void exhausted_case(void) {
    const long s[]={-EPIPE,-EPIPE,-EPIPE,-EPIPE,-EPIPE};reset(s,5,0);dc_io_playback_backend b=backend();dc_io_player *p=dc_io_player_create(&b);CHECK(p);dc_tag tag={.session_epoch=3,.reply_generation=1};int16_t pcm[160]={0};
    CHECK(dc_io_player_begin(p,tag,16000)==0);CHECK(dc_io_player_write(p,tag,pcm,sizeof pcm)==0);
    int found=0;for(int i=0;i<300&&!found;i++){dc_io_event e;while(dc_io_player_next_event(p,&e)==1)if(e.type==DC_IO_ERROR){CHECK(e.error==EPIPE);found=1;}if(!found){struct timespec t={0,10000000};nanosleep(&t,NULL);}}
    CHECK(found);pthread_mutex_lock(&mu);CHECK(prepares==4 && cursor==5 && observed_n==0);pthread_mutex_unlock(&mu);dc_io_player_destroy(p);puts("PASS five EPIPE returns: four prepares then EPIPE error");
}
int main(void) {
    const long fragment[]={1,-EAGAIN,-EINTR,0,159};run_case(fragment,5,0);
    const long pipe[]={1,-EPIPE,159};run_case(pipe,3,1);
    const long suspend[]={1,-ESTRPIPE,159};run_case(suspend,3,1);
    failure_case();exhausted_case();return 0;
}
