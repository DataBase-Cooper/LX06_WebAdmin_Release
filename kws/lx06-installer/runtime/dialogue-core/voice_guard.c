#define _POSIX_C_SOURCE 200809L
#include "voice_guard.h"
#include <fvad.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#define REF 4096u
#define WINDOW 120u
struct dc_voice_guard {
    Fvad *vad;pthread_mutex_t lock;
    int16_t reference[REF];uint64_t reference_at[REF];unsigned char reference_break[REF];
    unsigned head,used,phase,reference_rate,break_next;
    uint64_t reference_ms;unsigned voice_ms;
    uint64_t span_session,span_generation,span_operation,span_next;
    unsigned span_valid;
};
dc_voice_guard *dc_voice_guard_create(void) {
    dc_voice_guard *g=calloc(1,sizeof *g);if(!g)return NULL;
    g->vad=fvad_new();
    if(!g->vad||fvad_set_sample_rate(g->vad,16000)||fvad_set_mode(g->vad,2))goto fail;
    if(pthread_mutex_init(&g->lock,NULL))goto fail;
    return g;
fail:if(g->vad)fvad_free(g->vad);free(g);return NULL;
}
void dc_voice_guard_destroy(dc_voice_guard *g) {
    if(!g)return;
    fvad_free(g->vad);pthread_mutex_destroy(&g->lock);memset(g,0,sizeof *g);free(g);
}
void dc_voice_guard_reset_reference(dc_voice_guard *g) {
    if(!g)return;
    pthread_mutex_lock(&g->lock);
    g->head=g->used=g->phase=g->reference_rate=g->break_next=g->span_valid=0;
    g->reference_ms=0;
    pthread_mutex_unlock(&g->lock);
}
static void reference(dc_voice_guard *g,const int16_t *pcm,size_t frames,unsigned rate,uint64_t now,
                      uint64_t session,uint64_t generation,uint64_t operation,uint64_t offset) {
    if(!g||!pcm||!frames||rate<8000||rate>48000||frames>480)return;
    pthread_mutex_lock(&g->lock);
    int valid=session&&generation&&operation&&offset<=UINT64_MAX-frames;
    int contiguous=valid&&g->span_valid&&g->span_session==session&&
        g->span_generation==generation&&g->span_operation==operation&&g->span_next==offset;
    /* A new write cannot make an expired or future ring current again. */
    if(g->reference_rate!=rate || (g->used && (now<g->reference_ms || now-g->reference_ms>=700))) {
        g->phase=0;g->reference_rate=rate;g->used=0;g->break_next=0;
    } else if(!contiguous) {
        g->phase=0;if(g->used)g->break_next=1;
    }
    g->span_valid=(unsigned)valid;
    if(valid){g->span_session=session;g->span_generation=generation;g->span_operation=operation;g->span_next=offset+frames;}
    for(size_t i=0;i<frames;i++) {
        g->phase+=4000;
        if(g->phase>=rate) {
            g->phase-=rate;g->reference[g->head]=pcm[i];g->reference_at[g->head]=now;
            g->reference_break[g->head]=(unsigned char)g->break_next;g->break_next=0;
            g->head=(g->head+1)%REF;if(g->used<REF)g->used++;
        }
    }
    g->reference_ms=now;pthread_mutex_unlock(&g->lock);
}
void dc_voice_guard_reference(dc_voice_guard *g,const int16_t *pcm,size_t frames,unsigned rate,uint64_t now) {
    reference(g,pcm,frames,rate,now,0,0,0,0);
}
void dc_voice_guard_reference_span(dc_voice_guard *g,const int16_t *pcm,size_t frames,unsigned rate,uint64_t now,
                                   uint64_t session,uint64_t generation,uint64_t operation,uint64_t offset) {
    reference(g,pcm,frames,rate,now,session,generation,operation,offset);
}
int dc_voice_guard_process(dc_voice_guard *g,const int16_t *pcm,size_t frames,double gain,int16_t *out,uint64_t now,dc_voice_result *result) {
    if(!g||!pcm||!out||!result||(frames!=160&&frames!=320&&frames!=480)||!isfinite(gain)||gain< -24||gain>24)return -1;
    memset(result,0,sizeof *result);
    int voice=fvad_process(g->vad,pcm,frames);if(voice<0)return -1;
    double multiplier=pow(10.0,gain/20.0),energy=0;
    int16_t mic[WINDOW],reference[REF];unsigned char valid[REF];unsigned count=(unsigned)frames/4,used;
    for(size_t i=0;i<frames;i++) {
        energy+=(double)pcm[i]*pcm[i];double n=pcm[i]*multiplier;
        if(n>32767){n=32767;result->clipped++;}if(n< -32768){n= -32768;result->clipped++;}out[i]=(int16_t)lrint(n);
        if((i&3)==3)mic[i/4]=pcm[i];
    }
    result->rms=(unsigned)sqrt(energy/frames);result->voiced=voice!=0;
    pthread_mutex_lock(&g->lock);used=g->used;
    for(unsigned i=0;i<used;i++) {
        unsigned pos=(g->head+REF-used+i)%REF;uint64_t at=g->reference_at[pos];
        reference[i]=g->reference[pos];valid[i]=(unsigned char)((now>=at && now-at<700)?1:0);
        if(g->reference_break[pos])valid[i]|=2;
    }
    pthread_mutex_unlock(&g->lock);
    /* Compare actual accepted output over a bounded lag range. This is echo
     * rejection on top of stock PNS, not a claim to replace acoustic AEC. */
    double best=0;
    if(used>=count) {
        double em=0;for(unsigned i=0;i<count;i++)em+=(double)mic[i]*mic[i];
        unsigned oldest=used>2800?used-2800:0;
        if(em>0)for(unsigned start=oldest;start+count<=used;start+=8) {
            double er=0,dot=0;unsigned i;
            for(i=0;i<count&&(valid[start+i]&1)&&(!i||!(valid[start+i]&2));i++){double r=reference[start+i];er+=r*r;dot+=r*mic[i];}
            if(i==count&&er>count*64.0){double corr=fabs(dot)/sqrt(er*em);if(corr>best)best=corr;}
        }
    }
    result->correlation=best;result->echo=best>=0.72;
    unsigned duration=(unsigned)frames/16;
    if(voice&&result->rms>=80&&!result->echo){if(g->voice_ms<200)g->voice_ms+=duration;}
    else g->voice_ms=0;
    result->near_end=g->voice_ms>=80;return 0;
}
