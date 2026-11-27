/* Research seam, not linked into production. This is a reject gate, not AEC. */
#include "residual_candidate.h"
#include <fvad.h>
#include <stdlib.h>
struct residual_candidate { Fvad *vad; uint64_t last; unsigned have,voice_ms; };
residual_candidate *residual_candidate_new(void) {
    residual_candidate *c=calloc(1,sizeof *c); if(!c)return NULL;
    c->vad=fvad_new();
    if(!c->vad||fvad_set_sample_rate(c->vad,16000)||fvad_set_mode(c->vad,2)) {residual_candidate_free(c);return NULL;}
    return c;
}
void residual_candidate_free(residual_candidate *c) {if(c){if(c->vad)fvad_free(c->vad);free(c);}}
int residual_candidate_process(residual_candidate *c,const int16_t *mic,const int16_t *residual,size_t n,uint64_t reference_ms,int *trigger) {
    if(!c||!mic||!residual||!trigger||(n!=160&&n!=320&&n!=480)||(c->have&&reference_ms<=c->last))return -1;
    if(c->have&&reference_ms-c->last>=700) {
        fvad_reset(c->vad);
        if(fvad_set_sample_rate(c->vad,16000)||fvad_set_mode(c->vad,2))return -1;
        c->voice_ms=0;
    }
    c->last=reference_ms;c->have=1;*trigger=0;
    double m=0,r=0;
    for(size_t i=0;i<n;i++){m+=(double)mic[i]*mic[i];r+=(double)residual[i]*residual[i];}
    /* Reject low-energy and divergent filters before invoking VAD; no amplification. */
    if(m<n*6400.0||r<n*6400.0||r>m*1.25) {c->voice_ms=0;return 0;}
    int voiced=fvad_process(c->vad,residual,n);if(voiced<0)return -1;
    if(voiced){if(c->voice_ms<200)c->voice_ms+=(unsigned)n/16;}else c->voice_ms=0;
    *trigger=c->voice_ms>=80;return 0;
}
