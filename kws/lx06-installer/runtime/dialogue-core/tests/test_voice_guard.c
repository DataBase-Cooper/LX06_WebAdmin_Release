#define _POSIX_C_SOURCE 200809L
#include "../voice_guard.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{assert(x);checks++;}while(0)
int main(void) {
 dc_voice_guard *g=dc_voice_guard_create();CHECK(g!=NULL);
 int16_t in[480],out[480];dc_voice_result r;
 for(unsigned i=0;i<480;i++)in[i]=(int16_t)(1200*sin(i*0.147)+300*cos(i*0.087));
 CHECK(dc_voice_guard_process(g,in,480,0,out,100,&r)==0&&!memcmp(in,out,sizeof in));
 CHECK(!r.echo&&r.correlation==0);
 CHECK(dc_voice_guard_process(g,in,480,6.020599913,out,130,&r)==0);
 CHECK(abs(out[23]-2*in[23])<=1);
 CHECK(dc_voice_guard_process(g,in,480,-6.020599913,out,160,&r)==0);
 CHECK(abs(out[23]*2-in[23])<=1);
 CHECK(dc_voice_guard_process(g,in,480,NAN,out,170,&r)==-1);
 CHECK(dc_voice_guard_process(g,in,480,24.01,out,180,&r)==-1);
 CHECK(dc_voice_guard_process(g,in,42,0,out,190,&r)==-1);
 dc_voice_guard_reference(g,in,480,16000,200);
 for(unsigned i=0;i<480;i++)in[i]/=4;
 CHECK(dc_voice_guard_process(g,in,480,0,out,210,&r)==0&&r.echo&&r.correlation>.99&&!r.near_end);
 CHECK(dc_voice_guard_process(g,in,480,0,out,1000,&r)==0&&!r.echo);
 for(unsigned i=0;i<480;i++)in[i]=i&1?32000:-32000;
 CHECK(dc_voice_guard_process(g,in,480,12,out,1030,&r)==0&&r.clipped==480);
 CHECK(out[0]==-32768&&out[1]==32767);
 dc_voice_guard_destroy(g);
 printf("{\"passed\":true,\"checks\":%u,\"actual_acoustic_echo_test\":false}\n",checks);return 0;
}
