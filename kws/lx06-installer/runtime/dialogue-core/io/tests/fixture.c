#define _GNU_SOURCE
#include "../io.h"
#include "../compat.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static atomic_uint assertions;
#define CHECK(expr) do{atomic_fetch_add(&assertions,1);if(!(expr)){fprintf(stderr,"fixture-failed line=%u\n",__LINE__);exit(1);}}while(0)
static uint64_t ns(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000u+(unsigned long)t.tv_nsec;}
static void pause_ms(unsigned ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static atomic_uint creates;
int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int __wrap_pthread_create(pthread_t*a,const pthread_attr_t*b,void*(*c)(void*),void*d){atomic_fetch_add(&creates,1);return __real_pthread_create(a,b,c,d);}
struct fake {pthread_mutex_t lock;atomic_int mode,entered,release,polls;unsigned opened,closed,dropped,prepared;size_t count;int16_t output[131072];};
static int f_open(void*x,unsigned rate){struct fake*f=x;CHECK(rate==16000||rate==24000);pthread_mutex_lock(&f->lock);f->opened++;pthread_mutex_unlock(&f->lock);return 0;}
static int f_poll(void*x,struct pollfd*p,unsigned n){struct fake*f=x;atomic_fetch_add(&f->polls,1);(void)p;(void)n;return 0;}
static ssize_t f_write(void*x,const int16_t*pcm,size_t frames){struct fake*f=x;int mode=atomic_load(&f->mode);
    if(mode==1)return -EAGAIN;
    if(mode==2){atomic_store(&f->mode,0);return -EPIPE;}
    if(mode==4){atomic_store(&f->entered,1);while(!atomic_load(&f->release))pause_ms(1);}
    if(mode==3&&frames>1)frames/=2;
    CHECK(frames<=240);pthread_mutex_lock(&f->lock);CHECK(f->count+frames<=sizeof(f->output)/2);memcpy(f->output+f->count,pcm,frames*2);f->count+=frames;pthread_mutex_unlock(&f->lock);return (ssize_t)frames;}
static int f_delay(void*x,long*d){(void)x;*d=0;return 0;}
static int f_prepare(void*x){struct fake*f=x;pthread_mutex_lock(&f->lock);f->prepared++;pthread_mutex_unlock(&f->lock);return 0;}
static int f_drop(void*x){struct fake*f=x;pthread_mutex_lock(&f->lock);f->dropped++;pthread_mutex_unlock(&f->lock);return 0;}
static void f_close(void*x){struct fake*f=x;pthread_mutex_lock(&f->lock);f->closed++;pthread_mutex_unlock(&f->lock);}
static dc_io_player *player(struct fake*f){memset(f,0,sizeof(*f));CHECK(!pthread_mutex_init(&f->lock,NULL));dc_io_playback_backend b={f,f_open,f_poll,f_write,f_delay,f_prepare,f_drop,f_close};dc_io_player*p=dc_io_player_create(&b);CHECK(p);return p;}
static dc_io_event event(dc_io_player*p,enum dc_io_event_type type,dc_tag tag){uint64_t end=ns()+3000000000u;dc_io_event e;while(ns()<end){while(dc_io_player_next_event(p,&e)>0){CHECK(e.monotonic_ns>0);CHECK(e.type!=DC_IO_ERROR);if(e.type==type){CHECK(dc_tag_equal(e.tag,tag));return e;}}struct pollfd wait={dc_io_player_event_fd(p),POLLIN,0};(void)poll(&wait,1,10);}CHECK(0);return (dc_io_event){0};}
static void end_player(dc_io_player*p,struct fake*f){dc_io_player_destroy(p);pthread_mutex_destroy(&f->lock);}
static void player_case(const char*name){struct fake f;dc_io_player*p=player(&f);dc_tag a={1,1,1},b={1,2,2};int16_t pcm[2048];for(unsigned i=0;i<2048;i++)pcm[i]=(int16_t)(i+1);
    CHECK(dc_io_player_begin(p,a,16000)==0);
    if(!strcmp(name,"player-fifo")||!strcmp(name,"player-partial")||!strcmp(name,"player-recover")){
        if(!strcmp(name,"player-partial"))atomic_store(&f.mode,3);
        if(!strcmp(name,"player-recover"))atomic_store(&f.mode,2);
        CHECK(!dc_io_player_write(p,a,pcm,2048));CHECK(!dc_io_player_write(p,a,pcm+1024,2048));CHECK(!dc_io_player_finish(p,a));
        event(p,DC_IO_DRAINED,a);CHECK(f.count==2048);CHECK(!memcmp(f.output,pcm,4096));if(!strcmp(name,"player-recover"))CHECK(f.prepared==1);
    }else if(!strcmp(name,"player-stop")||!strcmp(name,"player-stale")||!strcmp(name,"player-backpressure")){
        atomic_store(&f.mode,1);for(unsigned i=0;i<16;i++)CHECK(!dc_io_player_write(p,a,pcm,sizeof(pcm)));
        if(!strcmp(name,"player-backpressure")){int r=0;for(unsigned i=0;i<4&&r==0;i++)r=dc_io_player_write(p,a,pcm,sizeof(pcm));CHECK(r==-EAGAIN);}
        uint64_t start=ns();CHECK(!dc_io_player_stop(p,a));CHECK(ns()-start<100000000u);CHECK(dc_io_player_queued(p)==0);CHECK(!dc_io_player_stop(p,a));event(p,DC_IO_STOPPED,a);CHECK(!dc_io_player_stop(p,a));
        CHECK(dc_io_player_write(p,a,pcm,320)==-ESTALE);
        CHECK(!dc_io_player_begin(p,b,24000));atomic_store(&f.mode,0);CHECK(!dc_io_player_write(p,b,pcm,480));CHECK(!dc_io_player_finish(p,b));event(p,DC_IO_DRAINED,b);CHECK(f.count==240);CHECK(f.dropped<=1);
    }else if(!strcmp(name,"player-old-stop-owner")){
        b.session_epoch=2; b.reply_generation=1; b.utterance_id=1;
        atomic_store(&f.mode,1);CHECK(!dc_io_player_write(p,a,pcm,320));
        CHECK(!dc_io_player_stop(p,a));event(p,DC_IO_STOPPED,a);
        CHECK(!dc_io_player_begin(p,b,16000));CHECK(!dc_io_player_write(p,b,pcm,320));
        CHECK(dc_io_player_stop(p,a)==-ESTALE);
        CHECK(dc_io_player_queued(p)==320);CHECK(!dc_io_player_write(p,b,pcm+160,320));
        atomic_store(&f.mode,0);CHECK(!dc_io_player_finish(p,b));event(p,DC_IO_DRAINED,b);
        CHECK(f.count==320);CHECK(!memcmp(f.output,pcm,640));
    }else if(!strcmp(name,"player-inflight-stop")){
        atomic_store(&f.mode,4);CHECK(!dc_io_player_write(p,a,pcm,4096));uint64_t end=ns()+2000000000u;while(!atomic_load(&f.entered)&&ns()<end)pause_ms(1);CHECK(atomic_load(&f.entered));
        uint64_t start=ns();CHECK(!dc_io_player_stop(p,a));CHECK(ns()-start<100000000u);CHECK(dc_io_player_queued(p)==0);
        atomic_store(&f.release,1);event(p,DC_IO_STOPPED,a);CHECK(f.count<=160);CHECK(f.dropped==1);
    }else if(!strcmp(name,"player-idle-no-spin")){
        pause_ms(120);CHECK(atomic_load(&f.polls)==0);CHECK(!dc_io_player_stop(p,a));event(p,DC_IO_STOPPED,a);
    }else if(!strcmp(name,"player-phrase-resume")){
        CHECK(!dc_io_player_write(p,a,pcm,320));event(p,DC_IO_FIRST_PCM,a);
        pause_ms(250);CHECK(!dc_io_player_write(p,a,pcm+160,320));event(p,DC_IO_PCM_RESUMED,a);
        CHECK(!dc_io_player_write(p,a,pcm+320,320));pause_ms(60);dc_io_event e;CHECK(dc_io_player_next_event(p,&e)==0);
        CHECK(!dc_io_player_finish(p,a));event(p,DC_IO_DRAINED,a);CHECK(f.count==480);
    }else if(!strcmp(name,"player-fixed-worker")){
        CHECK(atomic_load(&creates)==1);for(unsigned i=0;i<30;i++){if(i){a.reply_generation++;CHECK(!dc_io_player_begin(p,a,16000));}CHECK(!dc_io_player_finish(p,a));event(p,DC_IO_DRAINED,a);}CHECK(atomic_load(&creates)==1);
    }else if(!strcmp(name,"player-validation")){
        CHECK(dc_io_player_write(p,a,pcm,1)==-EINVAL);CHECK(dc_io_player_write(p,a,pcm,4098)==-EINVAL);CHECK(dc_io_player_write(p,b,pcm,320)==-ESTALE);CHECK(dc_io_player_begin(p,b,16000)==-EBUSY);CHECK(dc_io_player_begin(p,b,12345)==-EINVAL);CHECK(!dc_io_player_stop(p,a));event(p,DC_IO_STOPPED,a);CHECK(dc_io_player_begin(p,a,16000)==-ESTALE);
    }else if(!strcmp(name,"player-events-overflow")){
        for(unsigned i=0;i<80;i++){a.reply_generation=i+1;uint64_t end=ns()+1000000000u;if(i){int r;do{r=dc_io_player_begin(p,a,16000);if(r==-EBUSY)pause_ms(1);}while(r==-EBUSY&&ns()<end);CHECK(!r);}CHECK(!dc_io_player_finish(p,a));pause_ms(25);}
        int overflow=0;dc_io_event e;while(dc_io_player_next_event(p,&e)>0)if(e.type==DC_IO_ERROR&&e.error==ENOBUFS)overflow=1;CHECK(overflow);
    }else CHECK(0);
    end_player(p,&f);
}
static void identity(int fd,char*text){char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);ssize_t n=readlink(path,text,95);CHECK(n>0);text[n]=0;}
static int light_read(int fd,unsigned timeout){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,(int)timeout)<=0)return -1;unsigned char c;return recv(fd,&c,1,MSG_DONTWAIT)==1?c:-1;}
static void lights_case(const char*name){int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair));char id[96];identity(pair[0],id);
    if(!strcmp(name,"lights-untrusted")){CHECK(!dc_io_lights_create(pair[0],"socket:[1]"));char file[]="/tmp/dc-untrusted-XXXXXX";int f=mkstemp(file);CHECK(f>=0);CHECK(!dc_io_lights_create(f,id));close(f);CHECK(!unlink(file));close(pair[0]);close(pair[1]);return;}
    if(!strcmp(name,"lights-latest")){unsigned char sent=99;while(send(pair[0],&sent,1,MSG_DONTWAIT)==1){}CHECK(errno==EAGAIN||errno==EWOULDBLOCK);}
    dc_io_lights*l=dc_io_lights_create(pair[0],id);CHECK(l);dc_tag a={10,1,1},b={11,1,1};
    CHECK(!dc_io_lights_set(l,a,1,DC_CONNECTING));
    if(!strcmp(name,"lights-active")){
        CHECK(light_read(pair[1],1000)==1);enum dc_state states[]={DC_LISTENING,DC_THINKING,DC_SPEAKING,DC_SPEAKING,DC_INTERRUPTED,DC_LISTENING,DC_SPEAKING,DC_CLOSING};
        for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);i++){CHECK(!dc_io_lights_set(l,a,i+2,states[i]));CHECK(light_read(pair[1],1000)>0);}CHECK(!dc_io_lights_set(l,a,20,DC_CLOSED));CHECK(light_read(pair[1],1000)==0);CHECK(dc_io_lights_set(l,a,21,DC_LISTENING)==-ESTALE);
    }else if(!strcmp(name,"lights-stale")){
        CHECK(light_read(pair[1],1000)>0);CHECK(!dc_io_lights_set(l,b,1,DC_LISTENING));CHECK(light_read(pair[1],1000)>0);CHECK(dc_io_lights_set(l,a,999,DC_CLOSED)==-ESTALE);CHECK(dc_io_lights_set(l,b,1,DC_CLOSED)==-ESTALE);CHECK(light_read(pair[1],50)==-1);
    }else if(!strcmp(name,"lights-latest")){
        CHECK(!dc_io_lights_set(l,a,2,DC_CLOSED));pause_ms(40);CHECK(dc_io_lights_failed(l)>0);CHECK(!dc_io_lights_set(l,b,1,DC_LISTENING));
        int c,seen=0;uint64_t end=ns()+2000000000u;while(ns()<end&&(c=light_read(pair[1],100))>=0){CHECK(c!=0);if(c==2){seen=1;break;}}CHECK(seen);
    }else if(!strcmp(name,"lights-failure")){
        CHECK(light_read(pair[1],1000)>0);close(pair[1]);pair[1]=-1;CHECK(!dc_io_lights_set(l,a,2,DC_SPEAKING));pause_ms(60);CHECK(dc_io_lights_failed(l)>0);CHECK(dc_io_lights_sent(l)==1);CHECK(!dc_io_lights_set(l,a,3,DC_LISTENING));
    }else if(!strcmp(name,"lights-fd-reuse")){
        close(pair[0]);pair[0]=-1;
        char file_name[]="/tmp/dc-fd-reuse-XXXXXX";
        int file=mkstemp(file_name);
        CHECK(file>=0);
        CHECK(light_read(pair[1],1000)>0);
        CHECK(!dc_io_lights_set(l,a,2,DC_LISTENING));
        CHECK(light_read(pair[1],1000)==2);
        close(file);CHECK(!unlink(file_name));
    }else if(!strcmp(name,"lights-destroy-active")){
        CHECK(light_read(pair[1],1000)>0);dc_io_lights_destroy(l);l=NULL;CHECK(light_read(pair[1],50)==-1);
    }else if(!strcmp(name,"lights-destroy-closed")){
        CHECK(light_read(pair[1],1000)>0);CHECK(!dc_io_lights_set(l,a,2,DC_CLOSED));dc_io_lights_destroy(l);l=NULL;
        CHECK(light_read(pair[1],1000)==0);CHECK(light_read(pair[1],50)==-1);
    }else if(!strcmp(name,"lights-two-segments")){
        CHECK(light_read(pair[1],1000)>0);struct fake f;dc_io_player*p=player(&f);int16_t pcm[160]={0};
        for(unsigned i=0;i<2;i++) {a.reply_generation=i+1;CHECK(!dc_io_lights_set(l,a,2+i*2,DC_SPEAKING));CHECK(light_read(pair[1],1000)>0);
            CHECK(!dc_io_player_begin(p,a,16000));CHECK(!dc_io_player_write(p,a,pcm,sizeof(pcm)));CHECK(!dc_io_player_finish(p,a));event(p,DC_IO_DRAINED,a);
            CHECK(light_read(pair[1],50)==-1);CHECK(!dc_io_lights_set(l,a,3+i*2,DC_THINKING));CHECK(light_read(pair[1],1000)>0);}
        CHECK(!dc_io_lights_set(l,a,10,DC_INTERRUPTED));CHECK(light_read(pair[1],1000)>0);CHECK(!dc_io_lights_set(l,a,11,DC_LISTENING));CHECK(light_read(pair[1],1000)>0);end_player(p,&f);
        CHECK(!dc_io_lights_set(l,a,12,DC_CLOSED));CHECK(light_read(pair[1],1000)==0);
    }else CHECK(0);
    dc_io_lights_destroy(l);if(pair[0]>=0)close(pair[0]);if(pair[1]>=0)close(pair[1]);
}
static void put16(unsigned char*p,unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void put32(unsigned char*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static void put64(unsigned char*p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(n>>(8*i));}
static size_t wire(unsigned char*p,int mode,uint64_t seq,unsigned samples,int invalid){unsigned head=mode==2?56:16;memset(p,0,1200);put32(p,mode==2?0x3241584cU:0x3141584cU);
    if(mode==2){put16(p+4,2);put16(p+6,56);put32(p+8,head+samples*2);put32(p+12,16000);put32(p+16,samples);put32(p+20,invalid?999:1);put32(p+24,invalid?0:(uint32_t)getpid());put64(p+32,invalid?0:1234);put64(p+40,seq);put64(p+48,invalid?0:123456);}else{put32(p+4,(uint32_t)seq);put32(p+8,16000);put32(p+12,samples*2);}
    for(unsigned i=0;i<samples*2;i++)p[head+i]=(unsigned char)(i%251);
    return head+samples*2;}
static void capture_case(const char*name){char temporary[]="/tmp/dc-io-capture-XXXXXX";CHECK(mkdtemp(temporary));int mode=strstr(name,"v2")||strstr(name,"stamped")?2:1;
    dc_io_capture*c=dc_io_capture_open_mode(temporary,"test",mode==2?DC_IO_CAPTURE_LXA2:DC_IO_CAPTURE_LXA1);CHECK(c);char path[108],lock[108];snprintf(path,sizeof(path),"%s/%s",temporary,mode==2?"account.a2":"test.sock");snprintf(lock,sizeof(lock),"%s/%s",temporary,mode==2?"account.a2.lock":"test.lock");
    int sender=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);CHECK(sender>=0);struct sockaddr_un sa={.sun_family=AF_UNIX};strcpy(sa.sun_path,path);CHECK(!connect(sender,(void*)&sa,sizeof(sa)));unsigned char packet[1200];dc_io_capture_frame frame;
    if(!strcmp(name,"capture-lock")){dc_io_stat before,after;CHECK(!dc_io_fstatat(AT_FDCWD,lock,&before,0));CHECK(!dc_io_capture_open(temporary,"test"));dc_io_capture_close(c);c=dc_io_capture_open(temporary,"test");CHECK(c);CHECK(!dc_io_fstatat(AT_FDCWD,lock,&after,0));CHECK(before.st_ino==after.st_ino&&before.st_dev==after.st_dev);}
    else if(!strcmp(name,"capture-stall")){CHECK(dc_io_capture_stalled(c,ns()+4000000000u));CHECK(!dc_io_capture_stalled(c,0));}
    else if(!strcmp(name,"capture-untrusted")){CHECK(!chmod(temporary,0711));CHECK(!chmod(path,0666));pid_t child=fork();CHECK(child>=0);if(!child){if(setuid(65534))_exit(2);size_t n=wire(packet,1,1,160,0);_exit(send(sender,packet,n,0)==(ssize_t)n?0:3);}int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));CHECK(dc_io_capture_next(c,&frame)==-EPERM);}
    else if(!strcmp(name,"capture-replaced-path")){CHECK(!unlink(path));int fd=open(path,O_CREAT|O_WRONLY,0600);CHECK(fd>=0);close(fd);dc_io_capture_close(c);c=NULL;dc_io_stat replacement;CHECK(!dc_io_fstatat(AT_FDCWD,path,&replacement,0)&&S_ISREG(replacement.st_mode));CHECK(!unlink(path));}
    else if(!strcmp(name,"capture-malformed")||!strcmp(name,"capture-v2-malformed")){
        size_t n=wire(packet,mode,1,160,0);packet[0]=0;CHECK(send(sender,packet,n,0)==(ssize_t)n);CHECK(dc_io_capture_next(c,&frame)==-EPROTO);n=wire(packet,mode,2,160,0);CHECK(send(sender,packet,n-1,0)==(ssize_t)n-1);CHECK(dc_io_capture_next(c,&frame)==-EPROTO);n=wire(packet,mode,3,160,0);CHECK(send(sender,packet,n,0)==(ssize_t)n);CHECK(dc_io_capture_next(c,&frame)==1&&(frame.flags&DC_IO_CAPTURE_READY));
    }else if(!strcmp(name,"capture-v2-invalid-metadata")){
        size_t n=wire(packet,2,1,320,1);CHECK(send(sender,packet,n,0)==(ssize_t)n);CHECK(dc_io_capture_next(c,&frame)==1);CHECK(frame.bytes==640&&frame.available_mono_ms==0&&frame.producer_pid==0);CHECK(frame.flags&DC_IO_CAPTURE_METADATA_INVALID);CHECK(!memcmp(frame.pcm,packet+56,640));
    }else if(!strcmp(name,"capture-v2-explicit")){
        char no_sock[108];snprintf(no_sock,sizeof(no_sock),"%s/test.sock",temporary);CHECK(access(no_sock,F_OK)&&errno==ENOENT);size_t n=wire(packet,1,1,160,0);CHECK(send(sender,packet,n,0)==(ssize_t)n);CHECK(dc_io_capture_next(c,&frame)==-EPROTO);
    }else {
        unsigned samples[]={160,320,480};for(unsigned i=0;i<3;i++){uint64_t seq=i==2?5:i+1;size_t n=wire(packet,mode,seq,samples[i],0);CHECK(send(sender,packet,n,0)==(ssize_t)n);CHECK(dc_io_capture_next(c,&frame)==1);CHECK(frame.bytes==samples[i]*2&&frame.rate==16000&&frame.sequence==seq);CHECK(!memcmp(frame.pcm,packet+(mode==2?56:16),frame.bytes));if(mode==2){CHECK(frame.available_mono_ms==123456&&frame.producer_birth_ticks==1234&&frame.origin==1);}else CHECK(frame.available_mono_ms==0&&frame.producer_pid==0);if(i==2)CHECK((frame.flags&DC_IO_CAPTURE_GAP)&&frame.lost==2);}
        CHECK(!dc_io_capture_next(c,&frame));CHECK(!dc_io_capture_stalled(c,frame.monotonic_ns));
    }
    dc_io_capture_close(c);close(sender);CHECK(!unlink(lock));CHECK(!rmdir(temporary));
}
int main(int argc,char**argv){CHECK(argc==2);CHECK(geteuid()==0);
    if(!strncmp(argv[1],"player-",7))player_case(argv[1]);
    else if(!strncmp(argv[1],"lights-",7))lights_case(argv[1]);
    else if(!strncmp(argv[1],"capture-",8))capture_case(argv[1]);
    else if(!strcmp(argv[1],"alsa-public-abi")){dc_io_playback_backend b;dc_io_alsa*a=dc_io_alsa_create("null",&b);CHECK(a);CHECK(!b.open(b.context,16000));int16_t pcm[160]={0};CHECK(b.write(b.context,pcm,160)==160);CHECK(!b.drop(b.context));b.close(b.context);dc_io_alsa_destroy(a);}
    else CHECK(0);
    printf("{\"passed\":true,\"assertions\":%u,\"threads_created\":%u}\n",atomic_load(&assertions),atomic_load(&creates));return 0;
}
