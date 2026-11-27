#include "../../../../hooks/xiaoai/wake-oem-ns.h"
#include <stdio.h>
#include <time.h>

static struct wake_oem_ns filter;
static int16_t speech[96000], comparison[96000];
static unsigned samples;
static int run(unsigned seed,int save,int include_speech,int expect_difference) {
    double in_energy[2]={0},out_energy[2]={0};
    unsigned n[2]={0};
    int16_t in[NS_HOP],out[NS_HOP];
    unsigned differences=0,used=0;
    if(wake_oem_ns_open(&filter)) return 1;
    clock_t start=clock();
    for(unsigned pos=0;pos<96000;pos+=NS_HOP) {
        for(unsigned i=0;i<NS_HOP;i++) {
            seed=seed*1664525u+1013904223u;
            int noise=(int)(seed>>24)-128;
            int value=include_speech && pos>=32000 ? speech[pos+i]/2+noise : noise;
            in[i]=(int16_t)value;
        }
        int result=wake_oem_ns_hop(&filter,in,out);
        if(result<0 || (result!=0)!=(pos>=6*NS_HOP)) return 2;
        if(!result) continue;
        unsigned first=pos-3*NS_HOP;
        unsigned group=first>=32000;
        for(unsigned i=0;i<NS_HOP;++i) {
            float original=filter.input[i];
            in_energy[group]+=(double)original*original;
            out_energy[group]+=(double)out[i]*out[i];
            if(save) comparison[used]=out[i];
            else if(used<samples && out[i]!=comparison[used]) ++differences;
            ++used;
            ++n[group];
        }
    }
    wake_oem_ns_close(&filter);
    if(save) samples=used;
    printf("mono speech=%d samples=%u different=%u noise_in=%.2f noise_out=%.2f speech_in=%.2f speech_out=%.2f cpu_ms=%.1f adapter_bytes=%zu\n",
           include_speech,used,differences,sqrt(in_energy[0]/n[0]),sqrt(out_energy[0]/n[0]),
           sqrt(in_energy[1]/n[1]),sqrt(out_energy[1]/n[1]),1000.*(clock()-start)/CLOCKS_PER_SEC,sizeof(filter));
    if(!used || (include_speech && out_energy[1]/n[1] < 100)) return 3;
    if(!save && ((differences!=0)!=expect_difference || used!=samples)) return 4;
    return 0;
}
int main(void) {
    FILE *file=fopen("/speech.pcm","rb");
    if(!file || fread(speech,sizeof(*speech),96000,file)!=96000) return 5;
    fclose(file);
    int result=run(42,1,1,0);
    if(!result) result=run(9709,0,0,1);
    if(!result) result=run(42,0,1,0);
    return result;
}
