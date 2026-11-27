/* Offline synthetic models, not a vendor implementation or device test. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
static char switch_path[512], event_path[512], session_path[512], epoch_path[512];
#define LX06_OFFICIAL_FILE switch_path
#define LX06_NATIVE_REQUEST "request"
#define LX06_NATIVE_EVENT event_path
#define LX06_SESSION_FILE session_path
#define LX06_IDLE_EPOCH_FILE epoch_path
static void *fixture_dlsym(void *, const char *);
#define dlsym fixture_dlsym
#include "native-wake-bridge.c"
#undef dlsym
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
struct slot { wake_callback fn; void *ctx; };
struct engine { struct slot slots[8]; unsigned n; };
static int append_model, ignore_null;
static void fake_register(void *p, wake_callback fn, void *ctx) {
    struct engine *e=p;
    if (!fn && ignore_null) return;
    if (append_model) { CHECK(e->n<8); e->slots[e->n++]=(struct slot){fn,ctx}; }
    else { e->slots[0]=(struct slot){fn,ctx}; e->n=1; }
}
static unsigned starts, wuws;
static void fake_start(void) { starts++; }
static void fake_wuw(unsigned f,const void *p,int n,unsigned o,void *x) { (void)f;(void)p;(void)n;(void)o;(void)x; wuws++; }
static void *fixture_dlsym(void *h,const char *n) {
    (void)h;
    if (!strcmp(n,"xaudio_register_callback")) return (void *)fake_register;
    if (!strcmp(n,"set_wakeup_status")) return (void *)fake_start;
    return NULL;
}
struct context { unsigned calls; int live; };
static unsigned stale_calls;
static void cb_a(void *p,unsigned event,float angle) { struct context *c=p; (void)event;(void)angle; c->calls++; if (!c->live) stale_calls++; }
static void cb_b(void *p,unsigned event,float angle) { cb_a(p,event,angle); }
static void emit(struct engine *e,unsigned i) { CHECK(i<e->n && e->slots[i].fn); e->slots[i].fn(e->slots[i].ctx,1,3.0f); }
static void request(void) {
    int fd=open(LX06_NATIVE_REQUEST,O_CREAT|O_TRUNC|O_WRONLY,0600); CHECK(fd>=0);
    ssize_t n=write(fd,"wake\n",5); CHECK(n==5); CHECK(close(fd)==0);
    for(int i=0;i<5;i++) lx06_native_poll();
}
int main(int argc,char **argv) {
    CHECK(setenv("LX06_NATIVE_DIAGNOSTIC","1",1)==0);
    CHECK(argc==2); append_model=!strcmp(argv[1],"append"); ignore_null=!strcmp(argv[1],"ignore-null");
    CHECK(append_model || ignore_null || !strcmp(argv[1],"replace") || !strcmp(argv[1],"same-fn"));
    const char *tmp=getenv("TMPDIR"); CHECK(tmp && *tmp); char dir[512];
    CHECK(snprintf(dir,sizeof(dir),"%s/native-variants-XXXXXX",tmp)>0); CHECK(mkdtemp(dir)); CHECK(chdir(dir)==0);
    CHECK(snprintf(switch_path,sizeof(switch_path),"%s/switch",dir)>0);
    CHECK(snprintf(event_path,sizeof(event_path),"%s/event",dir)>0);
    CHECK(snprintf(session_path,sizeof(session_path),"%s/session",dir)>0);
    CHECK(snprintf(epoch_path,sizeof(epoch_path),"%s/epoch",dir)>0);
    int fd=open(switch_path,O_CREAT|O_WRONLY|O_TRUNC,0600); CHECK(fd>=0); CHECK(write(fd,"0",1)==1); CHECK(close(fd)==0);
    struct engine e={0}; struct context a={0,1},b={0,1};
    xaudio_register_callback(&e,cb_a,&a);
    if (!strcmp(argv[1],"same-fn")) {
        xaudio_register_callback(&e,cb_a,&b); CHECK(e.n==1); emit(&e,0); CHECK(a.calls==0 && b.calls==0);
        upload_wuw=fake_wuw; unsigned char pcm[320]={0}; lx06_native_audio(pcm,sizeof(pcm));
        request(); CHECK(a.calls==0 && b.calls==2 && starts==1 && wuws==1);
    } else if (ignore_null) {
        xaudio_register_callback(&e,NULL,NULL); CHECK(e.n==1 && e.slots[0].fn==forward_wake);
        a.live=0; upload_wuw=fake_wuw; unsigned char pcm[320]={0}; lx06_native_audio(pcm,sizeof(pcm));
        request(); CHECK(a.calls==0 && stale_calls==0 && starts==0 && wuws==0);
    } else {
        xaudio_register_callback(&e,cb_b,&b);
        if (append_model) { CHECK(e.n==2); emit(&e,0); CHECK(a.calls==0); emit(&e,1); }
        else { CHECK(e.n==1); emit(&e,0); }
        CHECK(a.calls==0 && b.calls==1 && wake_serial==0);
        a.live=0; upload_wuw=fake_wuw; unsigned char pcm[320]={0}; lx06_native_audio(pcm,sizeof(pcm));
        request(); CHECK(a.calls==0 && stale_calls==0 && b.calls==1 && starts==0 && wuws==0);
    }
    CHECK(unlink(switch_path)==0); (void)unlink(event_path); (void)unlink(session_path);
    (void)unlink(epoch_path); CHECK(unlink(LX06_NATIVE_REQUEST ".result")==0); CHECK(rmdir(dir)==0);
    printf("PASS synthetic %s: explicit modeled backend; no vendor/device inference\n",argv[1]);
    return 0;
}
