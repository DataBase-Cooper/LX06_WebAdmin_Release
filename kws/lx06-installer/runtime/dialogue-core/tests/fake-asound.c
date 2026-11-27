/* Synthetic timed public libasound ABI for the real reactor fixture. This
 * library opens no hardware, network or device node. Queue time is real mono
 * time; the append-only ledger records accepted PCM/drop boundaries. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifdef DC_FAKE_CLOCK
#include <stdatomic.h>
/* A separate preload build forces loop snapshot < teardown timestamp. This
 * deterministic forward-only clock fixture is enabled for warm-close only. */
int clock_gettime(clockid_t which,struct timespec *stamp){int r=(int)syscall(SYS_clock_gettime,which,stamp);if(!r&&which==CLOCK_MONOTONIC){static atomic_uint_fast64_t extra;uint64_t delta=atomic_fetch_add(&extra,UINT64_C(1000000));uint64_t nanos=(uint64_t)stamp->tv_nsec+delta;stamp->tv_sec+=(time_t)(nanos/UINT64_C(1000000000));stamp->tv_nsec=(long)(nanos%UINT64_C(1000000000));}return r;}
#else
typedef unsigned long frames_t;
typedef struct fake_pcm {int timer;unsigned id,rate,capacity;double queued;uint64_t at;} pcm_t;
typedef struct fake_sw {int unused;} sw_t;
static unsigned next_id;
static uint64_t ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(unsigned long)t.tv_nsec;}
static void record(pcm_t*p,const char*op,size_t frames,int sample){const char*path=getenv("DC_FAKE_ALSA_LEDGER");if(!path)return;int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);if(fd<0)return;char b[256];int n=snprintf(b,sizeof b,"{\"op\":\"%s\",\"at_ns\":%llu,\"id\":%u,\"frames\":%zu,\"sample\":%d}\n",op,(unsigned long long)ns(),p->id,frames,sample);if(n>0&&(size_t)n<sizeof b)(void)write(fd,b,(size_t)n);close(fd);}
static void advance(pcm_t*p){uint64_t now=ns();if(now>=p->at){p->queued-=(double)(now-p->at)*p->rate/1e9;if(p->queued<0)p->queued=0;}p->at=now;}
int snd_pcm_open(pcm_t**out,const char*device,int stream,int mode){if(!out||!device||stream||mode!=1)return -EINVAL;pcm_t*p=calloc(1,sizeof *p);if(!p)return -ENOMEM;p->timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);if(p->timer<0){free(p);return -errno;}struct itimerspec t={{0,1000000},{0,1000000}};if(timerfd_settime(p->timer,0,&t,NULL)){int e=errno;close(p->timer);free(p);return -e;}p->id=++next_id;p->at=ns();*out=p;record(p,"OPEN",0,0);return 0;}
int snd_pcm_close(pcm_t*p){if(p){record(p,"CLOSE",0,0);close(p->timer);free(p);}return 0;}
int snd_pcm_set_params(pcm_t*p,int format,int access,unsigned channels,unsigned rate,int resample,unsigned latency){if(!p||format!=2||access!=3||channels!=1||!rate||resample!=1||latency!=80000)return -EINVAL;p->rate=rate;p->capacity=rate*80/1000;return 0;}
int snd_pcm_sw_params_malloc(sw_t**p){*p=calloc(1,sizeof **p);return *p?0:-ENOMEM;}
void snd_pcm_sw_params_free(sw_t*p){free(p);}
int snd_pcm_sw_params_current(pcm_t*p,sw_t*s){return p&&s?0:-EINVAL;}
int snd_pcm_sw_params_set_start_threshold(pcm_t*p,sw_t*s,frames_t n){return p&&s&&n==1?0:-EINVAL;}
int snd_pcm_sw_params_set_avail_min(pcm_t*p,sw_t*s,frames_t n){return p&&s&&n==p->rate/100?0:-EINVAL;}
int snd_pcm_sw_params(pcm_t*p,sw_t*s){return p&&s?0:-EINVAL;}
int snd_pcm_poll_descriptors_count(pcm_t*p){return p?1:-EINVAL;}
int snd_pcm_poll_descriptors(pcm_t*p,struct pollfd*f,unsigned n){if(!p||!f||n!=1)return -EINVAL;*f=(struct pollfd){p->timer,POLLIN,0};return 1;}
long snd_pcm_writei(pcm_t*p,const void*data,frames_t frames){if(!p||!data)return -EINVAL;uint64_t tick;(void)read(p->timer,&tick,sizeof tick);advance(p);unsigned room=p->queued>=p->capacity?0:p->capacity-(unsigned)p->queued;if(!room)return -EAGAIN;if(frames>room)frames=room;p->queued+=frames;record(p,"WRITE",frames,*(const int16_t*)data);return (long)frames;}
int snd_pcm_delay(pcm_t*p,long*frames){if(!p||!frames)return -EINVAL;advance(p);*frames=(long)(p->queued+0.999);return 0;}
int snd_pcm_prepare(pcm_t*p){if(!p)return -EINVAL;p->queued=0;p->at=ns();record(p,"PREPARE",0,0);return 0;}
int snd_pcm_drop(pcm_t*p){if(!p)return -EINVAL;p->queued=0;p->at=ns();record(p,"DROP",0,0);return 0;}
#endif
