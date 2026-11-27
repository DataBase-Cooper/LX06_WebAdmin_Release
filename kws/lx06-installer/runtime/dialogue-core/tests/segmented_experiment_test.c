/* Offline hypothesis only: synthetic signals, no device or physical claims. */
#include "../voice_guard.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error assertions required
#endif
#include <assert.h>

typedef struct segmented segmented;
extern segmented *seg_create(void) __attribute__((weak));
extern void seg_free(segmented *) __attribute__((weak));
extern int seg_begin(segmented *,uint64_t,uint64_t) __attribute__((weak));
extern int seg_reference(segmented *,const int16_t *,size_t,uint64_t,uint64_t,uint64_t) __attribute__((weak));
extern int seg_process_fenced(segmented *,const int16_t *,uint64_t,uint64_t,uint64_t,dc_voice_result *) __attribute__((weak));
static void wave(int16_t *p,int n,unsigned seed){uint32_t s=seed;double x=0;for(int i=0;i<n;i++){s=s*1664525u+1013904223u;x=.86*x+.14*((int)(s>>16)-32768);p[i]=(int16_t)(x*.7);}}
int main(void){
 int16_t far[480],out[480]; wave(far,480,23);
 dc_voice_guard *base=dc_voice_guard_create();assert(base);
 for(int i=0;i<3;i++)dc_voice_guard_reference(base,far+i*160,160,16000,1000);
 dc_voice_result r;assert(!dc_voice_guard_process(base,far,480,0,out,1000,&r));
 assert(!r.echo);dc_voice_guard_destroy(base);
 if(!seg_create||!seg_process_fenced){fprintf(stderr,"RED: fenced segmented comparison candidate not implemented\n");return 1;}
 segmented *c=seg_create();assert(c);assert(seg_begin(c,1,1)==0);
 for(int i=0;i<3;i++)assert(seg_reference(c,far+i*160,160,1000,1,1)==0);
 assert(seg_process_fenced(c,far,1000,1,1,&r)==0);if(!r.echo){fprintf(stderr,"RED: disjoint individually matched 10ms chunks were not recognized\n");return 1;}
 assert(seg_begin(c,1,1)<0);assert(seg_begin(c,1,2)==0);
 assert(seg_reference(c,far,160,1000,1,1)<0);
 assert(seg_reference(c,far,160,1000,2,1)<0);
 assert(seg_process_fenced(c,far,1000,1,1,&r)<0);
 assert(seg_process_fenced(c,far,1000,1,2,&r)==0);assert(!r.echo);
 assert(seg_begin(c,1,1)<0);assert(seg_begin(c,2,1)==0);
 for(int i=0;i<3;i++)assert(seg_reference(c,far+i*160,160,1000,2,1)==0);
 assert(seg_process_fenced(c,far,1000,2,1,&r)==0);assert(r.echo);
 assert(seg_process_fenced(c,far,999,2,1,&r)==0);assert(!r.echo);
 assert(seg_process_fenced(c,far,1700,2,1,&r)==0);assert(!r.echo);
 seg_free(c);puts("segmented basic and sequential generation fences: PASS");return 0;
}
