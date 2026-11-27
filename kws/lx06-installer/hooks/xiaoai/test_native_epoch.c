/* Production native consumer against observed stock callback and real flock. */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
static void *fixture_dlsym(void *, const char *);
#define dlsym fixture_dlsym
#include "native-wake-bridge.c"
#undef dlsym

static const unsigned char source[16]={0x51}, other_source[16]={0x52};
static unsigned char issued[16];
static unsigned calls, expected_count, asr_calls;
static int native_asr, stock_transition;
static const unsigned char live_audio[320]={99};

static void assert_owned(void) {
    int fd=lx06_session_take_idle();
    assert(fd<0 && (errno==EWOULDBLOCK || errno==EAGAIN));
    unsigned char current[16];
    assert(!lx06_idle_epoch_read(current) && memcmp(current,issued,sizeof(current)));
}
static void consumer_wake(void *context,unsigned event,float angle) {
    (void)context; (void)angle;
    assert_owned();
    if(stock_transition) assert(event==1 && calls==0);
    else assert((calls==0 && event==0x101) || (calls==1 && event==1));
    ++calls;
}
static void consumer_wuw(unsigned flags,const void *data,int bytes,unsigned offset,void *extra) {
    const unsigned char *pcm=data;
    assert_owned();
    if(stock_transition) {
        assert(flags==1 && bytes==sizeof(live_audio) && !memcmp(pcm,live_audio,sizeof(live_audio)));
        ++calls;
        return;
    }
    assert(calls++==2 && flags==1 && !offset && !extra);
    assert(bytes==(int)(expected_count*2));
    for(int i=0;i<bytes;i++) assert(pcm[i]==0);
}
static void consumer_asr(void *context,const void *pcm,int bytes) {
    (void)context;
    assert(bytes==sizeof(live_audio) && !memcmp(pcm,live_audio,sizeof(live_audio)));
    ++asr_calls;
}
static int asr_flag(void) { return native_asr; }
static void consumer_start(void) { assert_owned(); assert(calls++==3); }
static void *fixture_dlsym(void *ignored,const char *name) {
    (void)ignored;
    if(!strcmp(name,"set_wakeup_status")) return consumer_start;
    if(!strcmp(name,"get_asr_flag")) return asr_flag;
    return NULL;
}

static void hex(char out[33],const unsigned char token[16]) {
    for(unsigned i=0;i<16;i++) snprintf(out+i*2,3,"%02x",token[i]);
}
static void new_epoch(void) {
    int lease=lx06_session_take_idle();
    assert(lease>=0 && !lx06_idle_epoch_changed(lease));
    close(lease);
    assert(!lx06_idle_epoch_snapshot(issued));
}
static void frames(const unsigned char from[16],unsigned first,unsigned end,uint64_t available) {
    unsigned char pcm[NATIVE_EPOCH_BYTES];
    for(unsigned i=first;i<end;i++) {
        for(unsigned j=0;j<NATIVE_EPOCH_SAMPLES;j++) {
            uint16_t value=(uint16_t)(i*NATIVE_EPOCH_SAMPLES+j);
            memcpy(pcm+j*2,&value,sizeof(value));
        }
        lx06_native_audio_epoch(pcm,from,issued,(uint64_t)i*NATIVE_EPOCH_SAMPLES,available);
    }
}
static void publish(const char *command) {
    FILE *f=fopen(LX06_NATIVE_REQUEST,"w");
    assert(f && fputs(command,f)>=0 && !fclose(f));
    for(unsigned i=0;i<5;i++) lx06_native_poll();
    assert(access(LX06_NATIVE_REQUEST,F_OK)!=0);
}
static void request(const unsigned char from[16],uint64_t first,uint64_t end,uint64_t deadline) {
    char epoch_hex[33],source_hex[33],command[160];
    hex(epoch_hex,issued); hex(source_hex,from);
    int n=snprintf(command,sizeof(command),"wake3 %s %s %llu %llu %llu\n",
                   epoch_hex,source_hex,(unsigned long long)first,(unsigned long long)end,
                   (unsigned long long)deadline);
    assert(n>0 && n<(int)sizeof(command));
    publish(command);
}

int main(void) {
    assert(!setenv("LX06_NATIVE_DIAGNOSTIC","1",1));
    callback=consumer_wake; upload_wuw=consumer_wuw; upload_asr=consumer_asr;
    lx06_native_audio(live_audio,sizeof(live_audio));
    assert(!unsetenv("LX06_NATIVE_DIAGNOSTIC"));
    publish("wake\n"); assert(!calls);

    new_epoch();
    int monitor = lx06_session_lock_idle(LOCK_SH);
    assert(monitor >= 0 && !lx06_session_busy());
    unsigned char observed[16];
    assert(!lx06_idle_epoch_snapshot(observed) && !memcmp(observed, issued, 16));
    assert(lx06_session_take_idle() < 0);
    close(monitor);
    uint64_t now=lx06_idle_epoch_now_ms();
    assert(now>0);
    const uint64_t end=157u*NATIVE_EPOCH_SAMPLES-51, first=end-16000;
    frames(source,0,160,now);  /* 3 later blocks must not enter WUW */
    request(other_source,first,end,now+3000); assert(!calls);
    request(source,0,end,now+3000); assert(!calls);
    request(source,end-9599,end,now+3000); assert(!calls);
    request(source,first,161u*NATIVE_EPOCH_SAMPLES,now+3000); assert(!calls);
    request(source,first,end,now-1); assert(!calls);
    request(source,first,end,now+3001); assert(!calls);
    request(source,first,end,now+2999); assert(!calls);
    native_asr=1; request(source,first,end,now+3000); assert(!calls); native_asr=0;
    int lease=lx06_session_take_idle(); assert(lease>=0);
    request(source,first,end,now+3000); assert(!calls); close(lease);

    expected_count=end-first;
    request(source,first,end,now+3000); assert(calls==4);
    forward_asr(NULL,live_audio,sizeof(live_audio)); assert(asr_calls==1);
    request(source,first,end,now+3000); assert(calls==4); /* previous epoch is retired */

    /* A full-length word can still be claimed near the end of its lease.
     * Later live frames wrap the ring while the claimed range remains valid. */
    new_epoch(); calls=0;
    now=lx06_idle_epoch_now_ms();
    for(unsigned i=0;i<320;i++) frames(source,i,i+1,now-(319u-i)*20u);
    const unsigned delayed_end=181u*NATIVE_EPOCH_SAMPLES-11;
    const uint64_t delayed_stamp=now-(319u-180u)*20u;
    expected_count=40000;
    request(source,delayed_end-expected_count,delayed_end,delayed_stamp+3000); assert(calls==4);

    new_epoch(); calls=0;
    frames(source,0,30,now);
    frames(other_source,30,31,now);
    request(source,0,30u*NATIVE_EPOCH_SAMPLES,now+3000); assert(!calls);
    frames(source,32,61,now); /* gap requires its own full window */
    request(source,31u*NATIVE_EPOCH_SAMPLES,61u*NATIVE_EPOCH_SAMPLES,now+3000); assert(!calls);
    frames(source,61,62,now);
    stock_transition=1;
    forward_wake(NULL,1,0); assert(calls==1);
    calls=0; stock_transition=0;
    request(source,32u*NATIVE_EPOCH_SAMPLES,62u*NATIVE_EPOCH_SAMPLES,now+3000); assert(!calls);

    new_epoch(); stock_transition=1;
    forward_wuw(1,live_audio,sizeof(live_audio),0,NULL); assert(calls==1);
    puts("PASS attested silent local handshake, live ASR, stock WUW, epoch and owner checks");
    return 0;
}
