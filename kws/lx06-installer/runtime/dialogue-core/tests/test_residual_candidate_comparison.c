/* Offline comparison only: production guard vs experimental residual gate.
 * See docs/voice-doubletalk-offline-comparison.md for build and limitations.
 * Synthetic signals are not speech; no device, network or recordings used. */
#include "../voice_guard.h"
#include "aec.h"
#include "residual_candidate.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME 480
#define BLOCKS 60
#define TRAIN 40
#define PI 3.14159265358979323846
static unsigned failures, far_failures, double_failures, candidate_far, candidate_mixed;
static uint32_t randword(uint32_t *s) { *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s; }
static void die(const char *s) { fprintf(stderr,"fixture error: %s\n",s); exit(2); }
/* Deterministic, independent multitone sources; continuous, not repeated frames. */
static void signals(int16_t *far, int16_t *near) {
    uint32_t a=12345,b=98765; double pa[16],pb[16];
    for(int j=0;j<16;j++){pa[j]=2*PI*(randword(&a)/(double)UINT32_MAX);pb[j]=2*PI*(randword(&b)/(double)UINT32_MAX);}
    for(int i=0;i<BLOCKS*FRAME;i++) {
        double x=0,y=0;
        for(int j=0;j<16;j++) {
            x+=sin(2*PI*(170+37*j)*i/16000.0+pa[j]);
            y+=sin(2*PI*(911+41*j)*i/16000.0+pb[j]);
        }
        far[i]=(int16_t)lrint(x*210); near[i]=(int16_t)lrint(y*150);
    }
}
static void run(const int16_t *far,const int16_t *near,int delay,double gain,int mixed) {
    dc_voice_guard *g=dc_voice_guard_create(); xz_aec a; residual_candidate *c=residual_candidate_new(); if(!c)die("candidate");
    if(!g||xz_aec_init(&a,16000,128,80,0.1f))die("init");
    int16_t r[FRAME],m[FRAME],out[FRAME],res[FRAME];
    double e0=0,e1=0,near_energy=0,near_cross=0; int trigger=0,echo=0,voiced=0,ctrigger=0;
    for(int k=0;k<BLOCKS;k++) {
        for(int i=0;i<FRAME;i++) {
            int n=k*FRAME+i; r[i]=far[n];
            int v=(n>=delay?(int)lrint(gain*far[n-delay]):0)+(mixed&&k>=TRAIN?near[n]:0);
            if(v>32767||v< -32768)die("clipped synthetic input");
            m[i]=(int16_t)v;
        }
        dc_voice_guard_reference(g,r,FRAME,16000,1000+30*k);
        dc_voice_result v;
        if(dc_voice_guard_process(g,m,FRAME,0,out,1000+30*k,&v)||xz_aec_process(&a,r,m,res,FRAME))die("process");
        int ct=0; if(residual_candidate_process(c,m,res,FRAME,1000+30*k,&ct))die("candidate process");
        if(k>=TRAIN) {
            ctrigger |= ct;
            trigger|=v.near_end; echo+=v.echo; voiced+=v.voiced;
            for(int i=0;i<FRAME;i++) {double n=near[k*FRAME+i];e0+=(double)m[i]*m[i];e1+=(double)res[i]*res[i];near_energy+=n*n;near_cross+=n*res[i];}
        }
    }
    printf("%s delay=%3d gain=%.2f guard_trigger=%d echo_frames=%d vad_frames=%d aec_out_in_rms=%.3f near_projection=%.3f\n",mixed?"double":"far   ",delay,gain,trigger,echo,voiced,sqrt(e1/e0),near_cross/near_energy);
    printf("candidate_trigger=%d\n",ctrigger); if(mixed&&!ctrigger)candidate_mixed++; if(!mixed&&ctrigger)candidate_far++;
    if (mixed&&!trigger) {failures++;double_failures++;}
    if (!mixed&&trigger) {failures++;far_failures++;}
    residual_candidate_free(c);xz_aec_destroy(&a);dc_voice_guard_destroy(g);
}
static void near_only(const int16_t *near) {
    dc_voice_guard *g=dc_voice_guard_create(); residual_candidate *c=residual_candidate_new(); if(!g||!c)die("guard"); int trigger=0,voiced=0,ctrigger=0; int16_t out[FRAME]; dc_voice_result v;
    for(int k=0;k<20;k++) {if(dc_voice_guard_process(g,near+k*FRAME,FRAME,0,out,1000+30*k,&v))die("near");trigger|=v.near_end;voiced+=v.voiced;int ct=0;if(residual_candidate_process(c,near+k*FRAME,near+k*FRAME,FRAME,1000+30*k,&ct))die("candidate near");ctrigger|=ct;}
    printf("near-only guard_trigger=%d vad_frames=%d\n",trigger,voiced); printf("near-only candidate_trigger=%d\n",ctrigger); if(!trigger)die("near-only VAD control did not trigger");residual_candidate_free(c);dc_voice_guard_destroy(g);
}
int main(void) {
    int16_t *far=malloc(BLOCKS*FRAME*sizeof(*far)),*near=malloc(BLOCKS*FRAME*sizeof(*near));if(!far||!near)die("alloc");signals(far,near);near_only(near);
    const int delays[]={0,80,200,480,960}; const double gains[]={0.25,0.75,1.5};
    for(unsigned d=0;d<sizeof delays/sizeof delays[0];d++)for(unsigned g=0;g<sizeof gains/sizeof gains[0];g++) {run(far,near,delays[d],gains[g],0);run(far,near,delays[d],gains[g],1);}
    printf("FAILURES far_false_trigger=%u double_missed=%u\n",far_failures,double_failures);
    printf("CANDIDATE far_false_trigger=%u double_missed=%u\n",candidate_far,candidate_mixed);free(far);free(near);return failures?1:0;
}
