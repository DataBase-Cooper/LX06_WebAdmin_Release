/* Offline synthetic diagnostic. Oracle-aligned AEC reference, not a live path. */
#include "../voice_guard.h"
#include "aec.h"
#include <speex/speex_echo.h>
#include <fvad.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define N 480
#define B 60
#define TRAIN 40
#define TOTAL (N*B)
#define PI 3.14159265358979323846
static uint32_t rnd(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
static void source(int16_t *out,int family,uint32_t seed) {
    uint32_t s=seed|1; double phase[12],freq[12],filtered=0;
    for(int j=0;j<12;j++){phase[j]=2*PI*rnd(&s)/4294967296.;freq[j]=180+1400*(rnd(&s)/4294967296.);}
    for(int i=0;i<TOTAL;i++){
        double x=0;
        if(family==0){for(int j=0;j<12;j++)x+=sin(2*PI*freq[j]*i/16000.+phase[j]);x*=180;}
        else {double noise=(rnd(&s)/2147483648.)-1.;filtered=.86*filtered+.14*noise;x=filtered*3200;}
        double env=family==0?1.0:(.3+.7*(.5+.5*sin(2*PI*i/8000.)));
        out[i]=(int16_t)lrint(x*env);
    }
}
static Fvad *vad_new(void){Fvad *v=fvad_new();if(!v||fvad_set_sample_rate(v,16000)||fvad_set_mode(v,2)){fprintf(stderr,"vad init\n");exit(2);}return v;}
static int vote(Fvad *v,int16_t *r,unsigned *ms){int q=fvad_process(v,r,N);if(q<0)exit(2);*ms=q?*ms+30:0;return *ms>=80;}
int main(int argc,char **argv){
 if(argc!=4)return 2;
 uint32_t seed=(uint32_t)strtoul(argv[1],0,10);int quick=atoi(argv[2]),stress=atoi(argv[3]);
 int16_t *far=malloc(TOTAL*sizeof *far),*near=malloc(TOTAL*sizeof *near);if(!far||!near)return 2;
 int delays[]={0,200,960};double gains[]={.35,1.2};int limit=quick?2:3;
 for(int ff=0;ff<2;ff++)for(int nf=0;nf<2;nf++){
 source(far,ff,seed+101u*(unsigned)(ff+1));source(near,nf,seed+100003u+211u*(unsigned)(nf+1));
 for(int di=0;di<limit;di++)for(int gi=0;gi<2;gi++)for(int kind=0;kind<3;kind++)for(int scenario=0;scenario<(stress?5:1);scenario++){
  int delay=delays[di];double gain=gains[gi];dc_voice_guard *g=dc_voice_guard_create();
  xz_aec legacy;if(!g||xz_aec_init(&legacy,16000,128,0,.1f))return 2;
  SpeexEchoState *sp=speex_echo_state_init(N,1024);if(!sp)return 2;int rate=16000;speex_echo_ctl(sp,SPEEX_ECHO_SET_SAMPLING_RATE,&rate);
  Fvad *sv=vad_new(),*lv=vad_new();unsigned sm=0,lm=0;int gt=0,st=0,lt=0,peak=0,gv=0,ge=0;double mic_e=0,se=0,le=0,ne=0,sn=0,ln=0;
  for(int k=0;k<B;k++){
   int16_t ref[N],aligned[N],mic[N],out[N],so[N],lo[N];
   for(int i=0;i<N;i++){int t=k*N+i;ref[i]=kind==2?0:far[t];
    int estimate=delay+(scenario==1?80:scenario==2?-80:scenario==3?160*t/TOTAL:0);
    if(estimate<0)estimate=0;
    aligned[i]=kind==2||t<estimate?0:far[t-estimate];
    /* Two-tap synthetic room; near starts only at evaluation. */
    double direct=t>=delay?far[t-delay]:0;
    int shifted=scenario==4&&k>=TRAIN;
    int tap=shifted?113:37;
    double echo=kind==2?0:gain*(shifted?.5:1)*(direct+(t>=delay+tap?(shifted?-.65:.22)*far[t-delay-tap]:0));
    int v=(int)lrint(echo)+(kind!=0&&k>=TRAIN?near[t]:0);if(v>32767||v< -32768){fprintf(stderr,"clipped\n");return 2;}mic[i]=(int16_t)v;if(abs(v)>peak)peak=abs(v);
   }
   dc_voice_guard_reference(g,ref,N,16000,1000+30*k);dc_voice_result r;
   if(dc_voice_guard_process(g,mic,N,0,out,1000+30*k,&r)||xz_aec_process(&legacy,aligned,mic,lo,N))return 2;
   speex_echo_cancellation(sp,mic,aligned,so);int vs=vote(sv,so,&sm),vl=vote(lv,lo,&lm);
   if(k>=TRAIN){gt|=r.near_end;st|=vs;lt|=vl;gv+=r.voiced;ge+=r.echo;
    for(int i=0;i<N;i++){double n=kind==0?0:near[k*N+i];mic_e+=(double)mic[i]*mic[i];se+=(double)so[i]*so[i];le+=(double)lo[i]*lo[i];ne+=n*n;sn+=n*so[i];ln+=n*lo[i];}}
  }
  printf("%d,%d,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%.9f,%.9f,%.9f,%.9f,%d\n",ff,nf,delay,gain,kind,peak,gt,st,lt,gv,ge,sqrt(se/(mic_e+1e-30)),sqrt(le/(mic_e+1e-30)),ne?sn/ne:0,ne?ln/ne:0,scenario);
  fvad_free(sv);fvad_free(lv);speex_echo_state_destroy(sp);xz_aec_destroy(&legacy);dc_voice_guard_destroy(g);
 }
 }
 free(far);free(near);return 0;
}
