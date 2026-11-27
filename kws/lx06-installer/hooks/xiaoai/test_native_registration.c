/* Offline host characterization of the actual native bridge. No device I/O.
 * Run via test_native_registration.sh; --require-gated is an expected RED. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
static char official_path[512], event_path[512], session_path[512], epoch_path[512];
#define LX06_OFFICIAL_FILE official_path
#define LX06_NATIVE_EVENT event_path
#define LX06_NATIVE_REQUEST "request"
#define LX06_SESSION_FILE session_path
#define LX06_IDLE_EPOCH_FILE epoch_path
static void *fixture_dlsym(void *, const char *);
#define dlsym fixture_dlsym
#include "native-wake-bridge.c"
#undef dlsym
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
struct engine { wake_callback fn; void *ctx; unsigned registrations; };
static void stock_register(void *opaque, wake_callback fn, void *ctx) {
    struct engine *e=opaque; e->fn=fn; e->ctx=ctx; e->registrations++;
}
static wuw_callback stock_wuw;
static unsigned wake_starts, wuw_calls, data_unregisters;
static void stock_upload_register(void *asr,void *wuw,void *c,void *d) {
    (void)asr; (void)c; (void)d; stock_wuw=(wuw_callback)wuw;
}
static void stock_upload_unregister(void) { stock_wuw=NULL; data_unregisters++; }
static void stock_start(void) { wake_starts++; }
static void test_wuw(unsigned flags,const void *pcm,int bytes,unsigned offset,void *extra) {
    (void)flags; (void)pcm; (void)bytes; (void)offset; (void)extra; wuw_calls++;
}
static void *fixture_dlsym(void *handle,const char *name) {
    (void)handle;
    if (!strcmp(name,"xaudio_register_callback")) return (void *)stock_register;
    if (!strcmp(name,"register_data_upload_callback")) return (void *)stock_upload_register;
    if (!strcmp(name,"unregister_data_upload_callback")) return (void *)stock_upload_unregister;
    if (!strcmp(name,"set_wakeup_status")) return (void *)stock_start;
    return NULL;
}
static void local_request(void) {
    int fd=open(LX06_NATIVE_REQUEST,O_WRONLY|O_CREAT|O_TRUNC,0600); CHECK(fd>=0);
    ssize_t n=write(fd,"wake\n",5); CHECK(n==5); CHECK(close(fd)==0);
    for(int i=0;i<5;i++) lx06_native_poll();
}
struct observed { unsigned calls,last; float angle; void *context; };
static struct observed a,b;
static void cb_a(void *ctx,unsigned event,float angle) { a.calls++; a.last=event; a.angle=angle; a.context=ctx; }
static void cb_b(void *ctx,unsigned event,float angle) { b.calls++; b.last=event; b.angle=angle; b.context=ctx; }
static void set_switch(char c) {
    int fd=open(official_path,O_WRONLY|O_CREAT|O_TRUNC,0600); CHECK(fd>=0);
    ssize_t n=write(fd,&c,1); CHECK(n==1); CHECK(close(fd)==0);
}
static void emit(struct engine *e,unsigned event,float angle) { CHECK(e->fn); e->fn(e->ctx,event,angle); }
int main(int argc,char **argv) {
    CHECK(setenv("LX06_NATIVE_DIAGNOSTIC","1",1)==0);
    int require=argc==2 && !strcmp(argv[1],"--require-gated");
    const char *tmp=getenv("TMPDIR"); CHECK(tmp && *tmp);
    char dir[512]; CHECK(snprintf(dir,sizeof(dir),"%s/native-reg-XXXXXX",tmp)>0); CHECK(mkdtemp(dir)); CHECK(chdir(dir)==0);
    CHECK(snprintf(official_path,sizeof(official_path),"%s/switch",dir)>0);
    CHECK(snprintf(event_path,sizeof(event_path),"%s/event",dir)>0);
    CHECK(snprintf(session_path,sizeof(session_path),"%s/session",dir)>0);
    CHECK(snprintf(epoch_path,sizeof(epoch_path),"%s/epoch",dir)>0);
    struct engine e1={0},e2={0}; int ctx1=11,ctx2=22;
    set_switch('1'); xaudio_register_callback(&e1,cb_a,&ctx1);
    CHECK(e1.fn && e1.fn!=cb_a && e1.ctx==&ctx1);
    emit(&e1,3,12.5f); CHECK(a.calls==1 && a.last==3 && a.context==&ctx1 && a.angle==12.5f);
    emit(&e1,1,31.25f); CHECK(a.calls==2 && a.last==1 && a.angle==31.25f);
    set_switch('0'); emit(&e1,1,20.5f); CHECK(a.calls==2);
    emit(&e1,3,13.5f); CHECK(a.calls==3 && a.last==3 && a.angle==13.5f);
    set_switch('1'); int fd=open(session_path,O_CREAT|O_RDWR,0600); CHECK(fd>=0); CHECK(flock(fd,LOCK_EX|LOCK_NB)==0);
    unsigned before=a.calls; emit(&e1,1,44.0f); CHECK(a.calls==before && official_pending==1);
    CHECK(flock(fd,LOCK_UN)==0); CHECK(close(fd)==0);
    set_switch('0'); for(int i=0;i<5;i++) lx06_native_poll(); CHECK(official_pending==0);
    register_data_upload_callback(NULL,(void *)test_wuw,NULL,NULL); CHECK(stock_wuw);
    unsigned char pcm[320]={0};
    stock_wuw(1,pcm,sizeof(pcm),0,NULL); CHECK(wuw_calls==0);
    set_switch('1'); stock_wuw(1,pcm,sizeof(pcm),0,NULL); CHECK(wuw_calls==1);
    set_switch('0'); lx06_native_audio(pcm,sizeof(pcm));
    before=a.calls; local_request(); CHECK(a.calls==before+2 && a.last==1 && a.context==&ctx1);
    CHECK(wuw_calls==2 && wake_starts==1);
    printf("PASS: busy primary defers and a single registered callback supports local wake with switch off\n");
    unregister_data_upload_callback(); CHECK(!stock_wuw && data_unregisters==1);
    before=a.calls; local_request(); CHECK(a.calls==before && wuw_calls==2 && wake_starts==1);
    register_data_upload_callback(NULL,(void *)test_wuw,NULL,NULL); CHECK(stock_wuw);
    printf("PASS: upload teardown prevents reuse of the unregistered WUW callback\n");
    xaudio_register_callback(&e1,cb_a,&ctx2); CHECK(e1.fn==forward_wake);
    before=a.calls; local_request(); CHECK(a.calls==before+2 && a.context==&ctx2);
    printf("PASS: same engine and callback can update the context\n");
    xaudio_register_callback(&e2,cb_b,&ctx2); CHECK(e2.fn && e2.ctx==&ctx2);
    unsigned serial_before=wake_serial;
    emit(&e2,1,42.5f); CHECK(wake_serial==serial_before);
    if(require) CHECK(b.calls==0); /* characterization of ordinary vendor callback */
    CHECK(b.calls==1 && b.context==&ctx2 && b.angle==42.5f);
    before=a.calls; local_request(); CHECK(a.calls==before && wuw_calls==3 && wake_starts==2);
    printf("PASS: ambiguous second registration preserves direct vendor callback and blocks local replay\n");
    /* Test backend supports NULL; whether the vendor does is not established. */
    xaudio_register_callback(&e1,NULL,NULL); CHECK(e1.fn==NULL && callback==cb_a);
    local_request(); CHECK(a.calls==before && wuw_calls==3 && wake_starts==2);
    printf("PASS: backend unregister cannot replay an old primary callback\n");
    CHECK(unlink(official_path)==0); (void)unlink(event_path); CHECK(unlink(session_path)==0);
    CHECK(unlink(epoch_path)==0); CHECK(unlink(LX06_NATIVE_REQUEST ".result")==0); CHECK(rmdir(dir)==0);
    return 0;
}
