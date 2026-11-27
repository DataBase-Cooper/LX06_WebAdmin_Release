"""Exercise actual native callback code with an isolated stock-ABI stub."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent
class NativeWakeTest(unittest.TestCase):
    def test_gating_handoff_and_callback_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            source=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>
#define LX06_SESSION_OWNER_H
#define LX06_SESSION_STATBUF struct stat
static int busy, preempts, native_asr;
static int lx06_session_busy(void) { return busy; }
static int lx06_session_take_idle(void) { return busy ? -1 : open("/dev/null", O_RDONLY|O_CLOEXEC); }
static int lx06_session_lock_idle(int mode) { (void)mode; return lx06_session_take_idle(); }
static int lx06_session_preempt(void) { ++preempts; return 1; }
static int fake_asr_active(void) { return native_asr; }
static int starts;
static void fake_start(void) { ++starts; }
static void *fake_dlsym(void *handle,const char *symbol) {
    (void)handle;
    if(!strcmp(symbol,"get_asr_flag")) return fake_asr_active;
    if(!strcmp(symbol,"set_wakeup_status")) return fake_start;
    return NULL;
}
#define dlsym fake_dlsym
#include "native-wake-bridge.c"
static int wakes, wuws, asrs, order;
static unsigned last_flags;
static void got_wake(void *ctx,unsigned event,float angle) {
    (void)ctx;(void)angle;
    assert(!busy); assert(event==0x101 || event==1);
    ++wakes; assert(++order==wakes);
}
static void got_wuw(unsigned flags,const void *pcm,int bytes,unsigned offset,void *extra) {
    (void)offset; assert(!extra); assert(!busy); assert(wakes==2);
    assert(++order==3); assert(bytes==320); assert(((const unsigned char *)pcm)[0]==42);
    ++wuws;last_flags=flags;
}
static void got_asr(void *ctx,const void *pcm,int bytes) {
    (void)ctx;(void)pcm;(void)bytes; ++asrs;
}
static void set_official(int on) {
    FILE *f=fopen(LX06_OFFICIAL_FILE,"w"); assert(f);fputc(on?'1':'0',f);fclose(f);
}
static void poll_once(void) { for(int i=0;i<5;i++) lx06_native_poll(); }
static void request_local(void) {
    FILE *f=fopen(LX06_NATIVE_REQUEST,"w");assert(f);fputs("wake\n",f);fclose(f);
}
static void reset(void) {
    wakes=wuws=asrs=order=starts=preempts=0;
    native_asr=0;script_active=busy=official_pending=deferred_bytes=poll_frames=0;
    stream_allowed=1;stream_source=0;set_official(1);
}
int main(void) {
    assert(!setenv("LX06_NATIVE_DIAGNOSTIC","1",1));
    callback=got_wake; upload_wuw=got_wuw; upload_asr=got_asr;
    unsigned char real_audio[320];memset(real_audio,42,sizeof(real_audio));
    reset();set_official(0);
    forward_wake(NULL,0x101,0);forward_wake(NULL,1,0);
    forward_wuw(1,real_audio,320,0,NULL);assert(!wakes && !wuws);
    lx06_native_audio(real_audio,sizeof(real_audio));request_local();poll_once();
    assert(wakes==2 && wuws==1 && last_flags==1 && starts==1);
    assert(access(LX06_NATIVE_REQUEST,F_OK)!=0);
    forward_asr(NULL,real_audio,320);assert(asrs==1);
    reset();busy=1;poll_once();request_local();poll_once();assert(!wakes);
    forward_wake(NULL,0x101,0);assert(!preempts && !wakes);
    forward_asr(NULL,real_audio,320);assert(!asrs);
    forward_wake(NULL,1,36);assert(preempts && !wakes);
    forward_wuw(0x101,real_audio,320,0,NULL);
    busy=0;poll_once();assert(!wakes && !wuws && official_pending);
    forward_wuw(1,real_audio,320,0,(void *)1);
    unsigned char suspect[320]={0};forward_wuw(0x201,suspect,320,0,(void *)2);
    request_local();
    poll_once();assert(wakes==2 && wuws==1 && last_flags==1 && !official_pending);
    poll_once();assert(wakes==2 && wuws==1 && !starts);
    forward_asr(NULL,real_audio,320);assert(asrs==1);
    reset();busy=1;forward_wake(NULL,1,0);forward_wuw(1,real_audio,320,0,NULL);
    set_official(0);busy=0;poll_once();assert(!wakes && !wuws && !official_pending);
    reset();request_local();
    struct utimbuf old={time(NULL)-10,time(NULL)-10};
    assert(!utime(LX06_NATIVE_REQUEST,&old));poll_once();
    assert(!wakes && !wuws && !starts && access(LX06_NATIVE_REQUEST,F_OK)!=0);
    FILE *forged=fopen(LX06_NATIVE_EVENT,"w");assert(forged);fputs("wake\n",forged);fclose(forged);
    assert(!symlink(LX06_NATIVE_EVENT,LX06_NATIVE_REQUEST));poll_once();
    assert(!wakes && !wuws && !starts && access(LX06_NATIVE_REQUEST,F_OK)!=0);
    reset();native_asr=1;request_local();poll_once();
    assert(wakes==2 && wuws==1 && starts==1 && stream_source==2);
    FILE *origin=fopen(LX06_NATIVE_EVENT,"r");assert(origin);
    char kind[32];assert(fscanf(origin,"%31s",kind)==1);fclose(origin);
    assert(!strcmp(kind,"local"));
    /* A later real official keyword restores authoritative official origin. */
    reset();forward_wake(NULL,1,0);
    origin=fopen(LX06_NATIVE_EVENT,"r");assert(origin);
    assert(fscanf(origin,"%31s",kind)==1);fclose(origin);
    assert(!strcmp(kind,"official"));
    puts("PASS official gate, local alias restart/origin, active-script exclusion, late confirmed WUW, suspect ordering, cancellation");
    return 0;
}
'''
            cfile=folder/'test.c';cfile.write_text(source)
            exe=folder/'test'
            defines=[f'-D{k}="{folder/name}"' for k,name in [('LX06_OFFICIAL_FILE','official'),('LX06_NATIVE_REQUEST','request'),('LX06_NATIVE_EVENT','event'),('LX06_SESSION_FILE','session')]]
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-I'+str(ROOT),*defines,str(cfile),'-ldl','-lpthread','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

    def test_attested_audio_is_checked_under_real_lease(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            exe=folder/'test'
            paths=[('LX06_OFFICIAL_FILE','official'),('LX06_NATIVE_REQUEST','request'),
                   ('LX06_NATIVE_EVENT','event'),('LX06_SESSION_FILE','session'),
                   ('LX06_OFFICIAL_BIND_PATH','bind'),('LX06_OFFICIAL_BIND_EVENT','bind-event')]
            defines=[f'-D{k}="{folder/name}"' for k,name in paths]
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-I'+str(ROOT),*defines,
                            str(ROOT/'test_native_epoch.c'),'-ldl','-lpthread','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

if __name__=='__main__': unittest.main()
