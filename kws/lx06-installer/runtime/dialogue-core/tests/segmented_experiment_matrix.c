/* Deterministic offline comparison. Synthetic multitone is not speech. */
#include "../voice_guard.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef NDEBUG
#error assertions required
#endif
#define FRAME 480
#define BLOCKS 60
#define TRAIN 40
#define PI 3.14159265358979323846
typedef struct segmented segmented;
segmented *seg_create(void);void seg_free(segmented *);
int seg_begin(segmented *,uint64_t,uint64_t);
int seg_reference(segmented *,const int16_t *,size_t,uint64_t,uint64_t,uint64_t);
int seg_process_fenced(segmented *,const int16_t *,uint64_t,uint64_t,uint64_t,dc_voice_result *);
static uint32_t rnd(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
static void signals(int16_t *far,int16_t *near){uint32_t a=12345,b=98765;double pa[16],pb[16];for(int j=0;j<16;j++){pa[j]=2*PI*(rnd(&a)/(double)UINT32_MAX);pb[j]=2*PI*(rnd(&b)/(double)UINT32_MAX);}for(int i=0;i<BLOCKS*FRAME;i++){double x=0,y=0;for(int j=0;j<16;j++){x+=sin(2*PI*(170+37*j)*i/16000.+pa[j]);y+=sin(2*PI*(911+41*j)*i/16000.+pb[j]);}far[i]=(int16_t)lrint(x*210);near[i]=(int16_t)lrint(y*150);}}
static unsigned far_false[2],mixed_miss[2],mixed_echo[2],far_echo[2];
static void run(const int16_t *far,const int16_t *near,int delay,double gain,int mixed){
 dc_voice_guard *g=dc_voice_guard_create();segmented *c=seg_create();assert(g&&c);assert(!seg_begin(c,1,1));int tr[2]={0},ec[2]={0},vf[2]={0};int16_t m[FRAME],out[FRAME];dc_voice_result r[2];
 for(int k=0;k<BLOCKS;k++){
  for(int i=0;i<FRAME;i++){int t=k*FRAME+i;int v=(t>=delay?(int)lrint(gain*far[t-delay]):0)+(mixed&&k>=TRAIN?near[t]:0);assert(v<=32767&&v>=-32768);m[i]=(int16_t)v;}
  for(int i=0;i<3;i++){dc_voice_guard_reference(g,far+k*FRAME+i*160,160,16000,1000+30*k);assert(!seg_reference(c,far+k*FRAME+i*160,160,1000+30*k,1,1));}
  assert(!dc_voice_guard_process(g,m,FRAME,0,out,1000+30*k,&r[0]));assert(!seg_process_fenced(c,m,1000+30*k,1,1,&r[1]));
  if(k>=TRAIN)for(int i=0;i<2;i++){tr[i]|=r[i].near_end;ec[i]+=r[i].echo;vf[i]+=r[i].voiced;}
 }
 for(int i=0;i<2;i++){if(mixed){mixed_miss[i]+=!tr[i];mixed_echo[i]+=ec[i];}else{far_false[i]+=tr[i];far_echo[i]+=ec[i];}}
 printf("%s delay=%d gain=%.2f base_trigger=%d cand_trigger=%d base_echo=%d cand_echo=%d base_vad=%d cand_vad_last=%d\n",mixed?"mixed":"far",delay,gain,tr[0],tr[1],ec[0],ec[1],vf[0],vf[1]);
 dc_voice_guard_destroy(g);seg_free(c);
}
int main(void){int16_t *far=malloc(BLOCKS*FRAME*sizeof *far),*near=malloc(BLOCKS*FRAME*sizeof *near);assert(far&&near);signals(far,near);const int delays[]={0,80,200,480,960};const double gains[]={.25,.75,1.5};for(unsigned d=0;d<5;d++)for(unsigned g=0;g<3;g++)for(int mixed=0;mixed<2;mixed++)run(far,near,delays[d],gains[g],mixed);printf("SUMMARY cases_per_type=15 eval_frames_per_case=20 baseline_far_false=%u candidate_far_false=%u baseline_mixed_missed=%u candidate_mixed_missed=%u baseline_far_echo_frames=%u candidate_far_echo_frames=%u baseline_mixed_echo_frames=%u candidate_mixed_echo_frames=%u\n",far_false[0],far_false[1],mixed_miss[0],mixed_miss[1],far_echo[0],far_echo[1],mixed_echo[0],mixed_echo[1]);free(far);free(near);return 0;}
