/* Gate the official client's synchronous playback submission, not a later log
 * observation. Built only for the verified stock mico_aivs_lab/libubus ABI.
 * Non-TTS calls and local cues pass through unchanged. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include "official-router-protocol.h"
#include "session-owner.h"
#ifndef OR_GATE_PID
#define OR_GATE_PID "/tmp/lx06-official-gate.pid"
#endif
__attribute__((constructor)) static void gate_loaded(void) {
    char executable[128];ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(length<=0)return;
    executable[length]=0;
    const char *base=strrchr(executable,'/');
    if(!base||strcmp(base+1,"mico_aivs_lab"))return;
    char data[96];int n=snprintf(data,sizeof(data),"%u %llu\n",(unsigned)getpid(),
                               (unsigned long long)lx06_process_ticks(getpid()));
    int f=open(OR_GATE_PID,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(f>=0){(void)write(f,data,(size_t)n);close(f);}
}
static int canceled(const char *id) {
    if(!*id)return 0;
    int f=open(OR_CANCELED,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return 0;
    uint32_t header[2];char key[96];
    if(read(f,header,sizeof(header))!=sizeof(header)||header[0]!=OR_MAGIC||header[1]>OR_MAX_CANCELED){close(f);return 0;}
    for(unsigned i=0;i<header[1];i++) {
        if(read(f,key,sizeof(key))!=sizeof(key))break;
        if(!strncmp(key,id,96)){close(f);return 1;}
    }
    close(f);
    return 0;
}
#ifdef __arm__
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
#endif
typedef int (*invoke_fn)(void *,uint32_t,const char *,void *,void *,void *,int,int);
/* libubox blobmsg wire format: network-order type/length, padded name header,
 * then value. Read only bounded STRING attributes; no dependency on its ABI. */
static int field(const void *blob,const char *key,char *out,size_t cap) {
    if(!blob) return 0;
    const unsigned char *b=blob;
    uint32_t raw;memcpy(&raw,b,4);
    size_t total=ntohl(raw)&0xffffff;
    if(total<4 || total>65536) return 0;
    for(size_t pos=4;pos+6<=total;) {
        memcpy(&raw,b+pos,4);raw=ntohl(raw);
        size_t len=raw&0xffffff;
        if(len<6 || len>total-pos) return 0;
        uint16_t nl;memcpy(&nl,b+pos+4,2);size_t name=ntohs(nl);
        size_t header=(2+name+1+3)&~(size_t)3;
        if((raw>>24&127)==3 && header+4<len && name==strlen(key) &&
           !memcmp(b+pos+6,key,name) && b[pos+6+name]==0) {
            const char *s=(const char *)b+pos+4+header;
            size_t n=len-4-header;
            const char *end=memchr(s,0,n);
            if(!end || (size_t)(end-s)>=cap) return 0;
            memcpy(out,s,(size_t)(end-s)+1);return 1;
        }
        pos+=(len+3)&~(size_t)3;
    }
    return 0;
}
int ubus_invoke_fd(void *ctx,uint32_t obj,const char *method,void *msg,
                   void *cb,void *priv,int timeout,int fd) {
    static invoke_fn original;
    invoke_fn invoke=__atomic_load_n(&original,__ATOMIC_ACQUIRE);
    if(!invoke){invoke=(invoke_fn)dlsym(RTLD_NEXT,"ubus_invoke_fd");__atomic_store_n(&original,invoke,__ATOMIC_RELEASE);}
    if(!invoke) return 9;
    if(method && !strcmp(method,"player_play_url")) {
        struct or_query q={.magic=OR_MAGIC,.action=OR_PLAY};
        field(msg,"dialog_id",q.dialog,sizeof(q.dialog));
        if(field(msg,"url",q.url,sizeof(q.url)) && strstr(q.url,"/tmp/tts/")) {
            int blocked=or_call(&q);
            syslog(LOG_INFO,"lx06-official-gate: submit dialog=%s blocked=%d",q.dialog,blocked);
            if(blocked==1 || (blocked<0 && canceled(q.dialog))) return 0;
        }
    }
    return invoke(ctx,obj,method,msg,cb,priv,timeout,fd);
}
