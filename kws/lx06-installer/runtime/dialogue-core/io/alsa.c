/* Opaque, public libasound ABI. This file does not copy ALSA implementation or
 * open the microphone. Only the player worker calls the returned methods. */
#define _GNU_SOURCE
#include "io.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
typedef struct _snd_pcm pcm_t;
typedef struct _snd_pcm_sw_params sw_t;
typedef unsigned long frames_t;
#ifdef __arm__
__asm__(".symver dlopen,dlopen@GLIBC_2.4");
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
__asm__(".symver dlclose,dlclose@GLIBC_2.4");
#endif
struct dc_io_alsa {
    void *library;
    pcm_t *pcm;
    char device[128];
    int (*pcm_open)(pcm_t **,const char *,int,int);
    int (*pcm_close)(pcm_t *);
    int (*set_params)(pcm_t *,int,int,unsigned,unsigned,int,unsigned);
    int (*sw_malloc)(sw_t **);
    void (*sw_free)(sw_t *);
    int (*sw_current)(pcm_t *,sw_t *);
    int (*sw_start)(pcm_t *,sw_t *,frames_t);
    int (*sw_avail)(pcm_t *,sw_t *,frames_t);
    int (*sw_apply)(pcm_t *,sw_t *);
    int (*poll_count)(pcm_t *);
    int (*poll_descriptors)(pcm_t *,struct pollfd *,unsigned);
    long (*writei)(pcm_t *,const void *,frames_t);
    int (*pcm_delay)(pcm_t *,long *);
    int (*pcm_prepare)(pcm_t *);
    int (*pcm_drop)(pcm_t *);
};
static int alsa_open(void *ctx,unsigned rate) {
    dc_io_alsa *a=ctx;sw_t *sw=NULL;
    /* PLAYBACK=0, NONBLOCK=1, S16_LE=2, RW_INTERLEAVED=3 are published ALSA
     * enum/flag values, identical on host and stock ARM hardfloat. */
    int r=a->pcm_open(&a->pcm,a->device,0,1);
    if(r<0){a->pcm=NULL;return r;}
    r=a->set_params(a->pcm,2,3,1,rate,1,80000);
    if(r<0)goto fail;
    r=a->sw_malloc(&sw);if(r<0)goto fail;
    if((r=a->sw_current(a->pcm,sw))<0 || (r=a->sw_start(a->pcm,sw,1))<0 ||
       (r=a->sw_avail(a->pcm,sw,rate/100u))<0 || (r=a->sw_apply(a->pcm,sw))<0)goto fail;
    a->sw_free(sw);return 0;
fail:
    if(sw)a->sw_free(sw);
    (void)a->pcm_drop(a->pcm);a->pcm_close(a->pcm);a->pcm=NULL;return r;
}
static int alsa_poll(void *ctx,struct pollfd *fds,unsigned capacity) {
    dc_io_alsa *a=ctx;if(!a->pcm)return -ENODEV;
    int n=a->poll_count(a->pcm);if(n<0)return n;
    if((unsigned)n>capacity)return -EOVERFLOW;
    return a->poll_descriptors(a->pcm,fds,(unsigned)n);
}
static ssize_t alsa_write(void *ctx,const int16_t *pcm,size_t frames) {
    dc_io_alsa *a=ctx;return a->pcm?a->writei(a->pcm,pcm,(frames_t)frames):-ENODEV;
}
static int alsa_delay(void *ctx,long *frames){dc_io_alsa *a=ctx;return a->pcm?a->pcm_delay(a->pcm,frames):-ENODEV;}
static int alsa_prepare(void *ctx){dc_io_alsa *a=ctx;return a->pcm?a->pcm_prepare(a->pcm):-ENODEV;}
static int alsa_drop(void *ctx){dc_io_alsa *a=ctx;return a->pcm?a->pcm_drop(a->pcm):0;}
static void alsa_close(void *ctx){dc_io_alsa *a=ctx;if(a->pcm){a->pcm_close(a->pcm);a->pcm=NULL;}}
dc_io_alsa *dc_io_alsa_create(const char *device,dc_io_playback_backend *out) {
    if(!device||!*device||strlen(device)>=128||!out){errno=EINVAL;return NULL;}
    dc_io_alsa *a=calloc(1,sizeof(*a));if(!a)return NULL;
    strcpy(a->device,device);a->library=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);
    if(!a->library){free(a);errno=ENOSYS;return NULL;}
#define LOAD(member,symbol) do { *(void **)(&a->member)=dlsym(a->library,symbol);if(!a->member)goto missing; } while(0)
    LOAD(pcm_open,"snd_pcm_open");LOAD(pcm_close,"snd_pcm_close");LOAD(set_params,"snd_pcm_set_params");
    LOAD(sw_malloc,"snd_pcm_sw_params_malloc");LOAD(sw_free,"snd_pcm_sw_params_free");
    LOAD(sw_current,"snd_pcm_sw_params_current");LOAD(sw_start,"snd_pcm_sw_params_set_start_threshold");
    LOAD(sw_avail,"snd_pcm_sw_params_set_avail_min");LOAD(sw_apply,"snd_pcm_sw_params");
    LOAD(poll_count,"snd_pcm_poll_descriptors_count");LOAD(poll_descriptors,"snd_pcm_poll_descriptors");
    LOAD(writei,"snd_pcm_writei");LOAD(pcm_delay,"snd_pcm_delay");
    LOAD(pcm_prepare,"snd_pcm_prepare");LOAD(pcm_drop,"snd_pcm_drop");
#undef LOAD
    *out=(dc_io_playback_backend){a,alsa_open,alsa_poll,alsa_write,alsa_delay,alsa_prepare,alsa_drop,alsa_close};return a;
missing:
    dlclose(a->library);free(a);errno=ENOSYS;return NULL;
}
void dc_io_alsa_destroy(dc_io_alsa *a){if(a){alsa_close(a);dlclose(a->library);free(a);}}
