/* Synthetic seam only. No ALSA or acoustic acceptance. */
#include "../voice_guard.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#ifdef NDEBUG
#error assertions required
#endif
/* Weak fallback exposes the old per-callback fence in the RED build. */
extern void dc_voice_guard_reference_span(dc_voice_guard *,const int16_t *,size_t,unsigned,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) __attribute__((weak));
static void span(dc_voice_guard *g,const int16_t *p,size_t n,uint64_t epoch,uint64_t gen,uint64_t op,uint64_t off){
 if(dc_voice_guard_reference_span)dc_voice_guard_reference_span(g,p,n,16000,1000,epoch,gen,op,off);
 else dc_voice_guard_reference(g,p,n,16000,1000);
}
static int echo(dc_voice_guard *g,const int16_t *p){int16_t out[160];dc_voice_result r;int rc=dc_voice_guard_process(g,p,160,0,out,1000,&r);assert(rc==0);return r.echo;}
int main(void){
 int16_t wave[160];for(unsigned i=0;i<160;i++)wave[i]=(int16_t)(1200*sin(i*.147)+300*cos(i*.087));
 dc_voice_guard *g=dc_voice_guard_create();assert(g);
 span(g,wave,1,1,1,1,0);span(g,wave+1,159,1,1,1,1);
 if(!echo(g,wave)){fprintf(stderr,"RED: contiguous 1+159 lost correlation\n");dc_voice_guard_destroy(g);return 1;}
 dc_voice_guard_destroy(g);
 for(unsigned kind=0;kind<5;kind++){
  g=dc_voice_guard_create();assert(g);span(g,wave,80,1,1,1,0);
  span(g,wave+80,80,kind==0?2:1,kind==1?2:1,kind==2?2:1,kind==3?81:kind==4?79:80);
  if(echo(g,wave)){fprintf(stderr,"incorrectly joined separated span kind %u\n",kind);return 1;}
  dc_voice_guard_destroy(g);
 }
 puts("continuity seam: PASS (synthetic only)");return 0;
}
