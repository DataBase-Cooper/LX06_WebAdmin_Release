#include "voice_guard.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int main(void){
 char line[256],name[64]=""; int16_t ref[160],mic[160],out[160],near[160],tmp[160];
 int t=0,k=0,vraw=0,vout=0,voiced=0,echo=0,peak=0,clipped=0;double me=0,oe=0,ne=0,np=0;dc_voice_guard *a=0,*b=0;
 puts("case,frames,samples,residual_rms_ratio,near_projection,guard_frames,raw_near_triggers,processed_near_triggers,processed_voiced,processed_echo,input_clipped,peak");
 while(fgets(line,sizeof line,stdin)){
  if(!strncmp(line,"CASE ",5)){
   if(a){if(t!=76800)return 3;printf("%s,300,38400,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d\n",name,sqrt(oe/(me+1e-30)),ne?np/ne:0,k,vraw,vout,voiced,echo,clipped,peak);dc_voice_guard_destroy(a);dc_voice_guard_destroy(b);}
   if(sscanf(line,"CASE %63s",name)!=1)return 2;
   a=dc_voice_guard_create();b=dc_voice_guard_create();if(!a||!b)return 2;
   t=k=vraw=vout=voiced=echo=peak=clipped=0;me=oe=ne=np=0;continue;
  }
  int r,m,o,n;if(sscanf(line,"S %d %d %d %d",&r,&m,&o,&n)!=4)continue;
  if(!a||t>=76800)return 2;int i=t%160;ref[i]=r;mic[i]=m;out[i]=o;near[i]=n;
  if(t>=38400){me+=(double)m*m;oe+=(double)o*o;ne+=(double)n*n;np+=(double)n*o;if(abs(m)>peak)peak=abs(m);clipped+=(m==32767||m== -32768);}
  t++;
  if(t%160==0){dc_voice_result x,y;unsigned now=1000+t/16;
   dc_voice_guard_reference(a,ref,160,16000,now);dc_voice_guard_reference(b,ref,160,16000,now);
   if(dc_voice_guard_process(a,mic,160,0,tmp,now,&x)||dc_voice_guard_process(b,out,160,0,tmp,now,&y))return 4;
   if(t>38400){k++;vraw+=x.near_end;vout+=y.near_end;voiced+=y.voiced;echo+=y.echo;}
  }
 }
 if(!a||t!=76800)return 3;
 printf("%s,300,38400,%.8f,%.8f,%d,%d,%d,%d,%d,%d,%d\n",name,sqrt(oe/(me+1e-30)),ne?np/ne:0,k,vraw,vout,voiced,echo,clipped,peak);
 dc_voice_guard_destroy(a);dc_voice_guard_destroy(b);return 0;
}
