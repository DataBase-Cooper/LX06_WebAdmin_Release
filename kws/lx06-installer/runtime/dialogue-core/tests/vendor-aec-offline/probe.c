#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
extern void *CreatAECHandler(void);
extern int AECProcess(void*,const int16_t*,const int16_t*,int,int16_t*,int*);
extern void DestroyAECHandler(void**);
#define FR 600
#define N (FR*128)
static int16_t far[N+2048], near[N+2048];
static uint32_t rng=431;
static int noise(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return (int)(rng%12001)-6000;}
static int16_t clip(double v){return v>32767?32767:v< -32768?-32768:(int16_t)lrint(v);}
int main(void){
 const char *names[]={"aligned_far","offset_minus80","offset_plus80","drift_plus200ppm","drift_minus200ppm","gain_step","clip","near_only","doubletalk","path_step"};
 for(int i=0;i<N+2048;i++){far[i]=noise();near[i]=noise()/2;}
 for(int c=0;c<10;c++){
  void*h=CreatAECHandler();if(!h)return 2;
  printf("CASE %s\n",names[c]);
  for(int f=0;f<FR;f++){
   int16_t mic[128],ref[128],out[128],n[128];
   for(int i=0;i<128;i++){
    int t=f*128+i, z=t+1024, shift=(c==1?-80:c==2?80:c==3?(int)lrint(t*.0002):c==4?-(int)lrint(t*.0002):0);
    int source=z-256, refidx=source+shift;
    ref[i]=c==7?0:far[refidx];
    double gain=(c==5&&f>=300)?1.25:.5;
    double echo=(c==7?0:gain*(far[source]+.22*far[source-37]));
    if(c==9&&f>=300)echo=.8*(far[source-73]-.65*far[source-186]);
    if(c==6)echo=far[source]*8.0;
    n[i]=(c==7||c==8)&&f>=300?near[z]:0;
    mic[i]=clip(echo+n[i]);out[i]=0;
   }
   int len=-1;int rc=AECProcess(h,mic,ref,1536,out,&len);
   if(rc||len!=256){fprintf(stderr,"AEC failed rc=%d len=%d\n",rc,len);return 3;}
   for(int i=0;i<128;i++)printf("S %d %d %d %d\n",ref[i],mic[i],out[i],n[i]);
  }
  DestroyAECHandler(&h);
 }
 return 0;
}
