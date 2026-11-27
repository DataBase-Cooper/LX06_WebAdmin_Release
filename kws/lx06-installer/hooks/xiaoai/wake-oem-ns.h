/* Independent, per-source 16 kHz mono stream. Reset every array and the
 * vendor NS instance before accepting another capture source or interval. */
#ifndef LX06_WAKE_OEM_NS_H
#define LX06_WAKE_OEM_NS_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { NS_WINDOW=512, NS_HOP=128 };
struct ns_bin { float real, imag; };
extern int WebRtcNs_Create(void **);
extern int WebRtcNs_Init(void *,unsigned);
extern int WebRtcNs_set_policy(void *,int);
extern int WebRtcNs_ProcessCore(void *,struct ns_bin *,struct ns_bin *);
extern int WebRtcNs_Free(void *);
extern void *kiss_fftr_alloc(int,int,void *,size_t *);
extern void kiss_fftr(void *,const float *,struct ns_bin *);
extern void kiss_fftri(void *,const struct ns_bin *,float *);
#ifdef LX06_WAKE_PAIRED_NS
/* The official executable loads the DSP library with its own dependencies.
 * Acquire those exports after the first real PCM read, never in the preload
 * object's ELF dependency list or before the official loader is ready. */
#include <dlfcn.h>
static struct {
    __typeof__(&WebRtcNs_Create) create;
    __typeof__(&WebRtcNs_Init) init;
    __typeof__(&WebRtcNs_set_policy) policy;
    __typeof__(&WebRtcNs_ProcessCore) process;
    __typeof__(&WebRtcNs_Free) release;
    __typeof__(&kiss_fftr_alloc) fft_alloc;
    __typeof__(&kiss_fftr) forward;
    __typeof__(&kiss_fftri) inverse;
    void *library;
} wake_ns_api;
static int wake_ns_resolve(void) {
    if(wake_ns_api.library) return 1;
    void *lib=dlopen("libxaudio_engine.so",RTLD_LAZY|RTLD_NOLOAD);
    if(!lib) return 0;
    wake_ns_api.create=dlsym(lib,"WebRtcNs_Create");
    wake_ns_api.init=dlsym(lib,"WebRtcNs_Init");
    wake_ns_api.policy=dlsym(lib,"WebRtcNs_set_policy");
    wake_ns_api.process=dlsym(lib,"WebRtcNs_ProcessCore");
    wake_ns_api.release=dlsym(lib,"WebRtcNs_Free");
    wake_ns_api.fft_alloc=dlsym(lib,"kiss_fftr_alloc");
    wake_ns_api.forward=dlsym(lib,"kiss_fftr");
    wake_ns_api.inverse=dlsym(lib,"kiss_fftri");
    if(!wake_ns_api.create || !wake_ns_api.init || !wake_ns_api.policy ||
       !wake_ns_api.process || !wake_ns_api.release || !wake_ns_api.fft_alloc ||
       !wake_ns_api.forward || !wake_ns_api.inverse) {
        dlclose(lib);
        return 0;
    }
    wake_ns_api.library=lib;
    return 1;
}
#define WebRtcNs_Create wake_ns_api.create
#define WebRtcNs_Init wake_ns_api.init
#define WebRtcNs_set_policy wake_ns_api.policy
#define WebRtcNs_ProcessCore wake_ns_api.process
#define WebRtcNs_Free wake_ns_api.release
#define kiss_fftr_alloc wake_ns_api.fft_alloc
#define kiss_fftr wake_ns_api.forward
#define kiss_fftri wake_ns_api.inverse
#endif
struct wake_oem_ns {
    void *handle, *forward, *inverse;
    float window[NS_WINDOW], input[NS_WINDOW], tail[NS_WINDOW], work[NS_WINDOW];
    struct ns_bin spectrum[NS_WINDOW/2+1];
    unsigned hops;
};
static void wake_oem_ns_close(struct wake_oem_ns *s) {
    if(s->handle) WebRtcNs_Free(s->handle);
    free(s->forward);
    free(s->inverse);
    memset(s,0,sizeof(*s));
}
static int wake_oem_ns_open(struct wake_oem_ns *s) {
    memset(s,0,sizeof(*s));
#ifdef LX06_WAKE_PAIRED_NS
    if(!wake_ns_resolve()) return -1;
#endif
    s->forward=kiss_fftr_alloc(NS_WINDOW,0,NULL,NULL);
    s->inverse=kiss_fftr_alloc(NS_WINDOW,1,NULL,NULL);
    if(!s->forward || !s->inverse || WebRtcNs_Create(&s->handle) ||
       WebRtcNs_Init(s->handle,16000) || WebRtcNs_set_policy(s->handle,0)) {
        wake_oem_ns_close(s);
        return -1;
    }
    for(unsigned i=0;i<NS_WINDOW;++i) s->window[i]=sinf(3.14159265358979323846f*i/NS_WINDOW);
    return 0;
}
/* At hop 7 the output covers original samples [384,512), with a
 * complete four-window overlap. Report sample positions in that domain. */
static int wake_oem_ns_hop(struct wake_oem_ns *s,const int16_t in[NS_HOP],int16_t out[NS_HOP]) {
    memmove(s->input,s->input+NS_HOP,(NS_WINDOW-NS_HOP)*sizeof(float));
    for(unsigned i=0;i<NS_HOP;++i) s->input[NS_WINDOW-NS_HOP+i]=in[i];
    if(s->hops<7) ++s->hops;
    if(s->hops<4) return 0;
    for(unsigned i=0;i<NS_WINDOW;++i) s->work[i]=s->input[i]*s->window[i];
    kiss_fftr(s->forward,s->work,s->spectrum);
    if(WebRtcNs_ProcessCore(s->handle,s->spectrum,s->spectrum)) return -1;
    kiss_fftri(s->inverse,s->spectrum,s->work);
    for(unsigned i=0;i<NS_WINDOW;++i) {
        float y=s->tail[i]+s->work[i]*s->window[i]/(2*NS_WINDOW);
        if(!isfinite(y)) return -1;
        s->tail[i]=y;
    }
    if(s->hops>=7) {
        for(unsigned i=0;i<NS_HOP;++i) {
            float y=fmaxf(-32768.f,fminf(32767.f,s->tail[i]));
            out[i]=(int16_t)lrintf(y);
        }
    }
    memmove(s->tail,s->tail+NS_HOP,(NS_WINDOW-NS_HOP)*sizeof(float));
    memset(s->tail+NS_WINDOW-NS_HOP,0,NS_HOP*sizeof(float));
    return s->hops>=7;
}
#endif
