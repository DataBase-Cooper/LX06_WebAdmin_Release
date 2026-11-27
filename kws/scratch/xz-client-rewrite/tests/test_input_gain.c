#include "input_gain.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    xz_input_gain state;
    int16_t original[]={INT16_MIN,-20000,-1000,0,1000,20000,INT16_MAX},pcm[7];
    const char *bypass[]={NULL,"无增益","off","0","invalid","4"};
    for(size_t j=0;j<sizeof(bypass)/sizeof(*bypass);j++) {
        xz_input_gain_init(&state,bypass[j]);memcpy(pcm,original,sizeof(pcm));
        xz_input_gain_apply(&state,pcm,7,1);assert(!memcmp(pcm,original,sizeof(pcm)));
    }
    for(unsigned preset=1;preset<=3;preset++) {
        const char *names[]={"无增益","低增益","中增益","高增益"};
        xz_input_gain_init(&state,names[preset]);assert(state.preset==preset);
        int16_t quiet[3];
        for(int frame=0;frame<80;frame++) {
            quiet[0]=-1000;quiet[1]=0;quiet[2]=1000;
            xz_input_gain_apply(&state,quiet,3,1);
        }
        int target=preset==1?1500:preset==2?2000:3000;
        assert(quiet[2]>=target-2 && quiet[2]<=target);
        assert(quiet[0]==-quiet[2] && quiet[1]==0);
        memcpy(pcm,original,sizeof(pcm));xz_input_gain_apply(&state,pcm,7,1);
        assert(pcm[0]>=-30000 && pcm[6]<=30000);
        assert(pcm[1]<pcm[2] && pcm[4]<pcm[5]); /* loud waveform not flattened */
        memcpy(pcm,original,sizeof(pcm));xz_input_gain_apply(&state,pcm,7,0);
        assert(!memcmp(pcm,original,sizeof(pcm))); /* playback exact bypass */
        memset(pcm,0,sizeof(pcm));xz_input_gain_apply(&state,pcm,7,1);
        for(int i=0;i<7;i++)assert(pcm[i]==0);
    }
    const char *bad[]={"nan","inf","12.01","-12.01","1.001","1e1","--1","1;echo"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        xz_input_gain_init_db(&state,bad[i],"高增益");assert(state.target==1.f);
    }
    const char *levels[]={"-12.00","-3.25","0.00","3.25","12.00"};
    for(size_t i=0;i<sizeof(levels)/sizeof(*levels);i++) {
        xz_input_gain_init_db(&state,levels[i],"高增益");
        for(int frame=0;frame<80;frame++) {pcm[0]=-1000;pcm[1]=0;pcm[2]=1000;xz_input_gain_apply(&state,pcm,3,1);}
        float expected=1000.f*powf(10.f,(float)atof(levels[i])/20.f);
        assert(fabsf(pcm[2]-expected)<2);assert(pcm[0]==-pcm[2]&&pcm[1]==0);
        memcpy(pcm,original,sizeof(pcm));xz_input_gain_apply(&state,pcm,7,0);
        assert(!memcmp(pcm,original,sizeof(pcm)));
    }
    puts("input gain dB/presets, bypass, silence and peak protection OK");return 0;
}
