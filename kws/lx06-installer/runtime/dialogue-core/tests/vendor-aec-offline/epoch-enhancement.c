/* Offline frozen-OEM component qualification. Only synthetic samples are used. */
#define _GNU_SOURCE
#include <math.h>
#include <malloc.h>
#include <time.h>
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void *CreatAECHandler(void);
extern int AECProcess(void *,const int16_t *,const int16_t *,int,int16_t *,int *);
extern void DestroyAECHandler(void **);
extern void *CreatAFEEnhancementHandler(const char *,unsigned);
extern void DestroyAFEEnhancementHandler(void **);
extern void AFEEnhancementProcess(void *,int16_t *,int,int16_t *,int *,int,float *);
extern int WebRtcNs_ProcessCore(void *,void *,void *);

static int bypass, invocations, alias_errors;
static uintptr_t vendor_base;
static void crash(int sig,siginfo_t *info,void *context) {
    ucontext_t *cpu=context;
    fprintf(stderr,"synthetic probe fault=%d relative_pc=%lx relative_caller=%lx address=%p\n",sig,
            (unsigned long)(cpu->uc_mcontext.arm_pc-vendor_base),
            (unsigned long)(cpu->uc_mcontext.arm_lr-vendor_base),info->si_addr);
    _exit(8);
}
/* The frozen vendor wrapper passes the same spectral buffer as input and
 * output. This diagnostic ablates exactly the NS step for the control run. */
int webrtc_postfilter(void *handle, void *input, void *output) {
    ++invocations;
    if (input != output) { ++alias_errors; return -1; }
    return bypass ? 0 : WebRtcNs_ProcessCore(handle,input,output);
}
static uint32_t generator=42;
static int noise(void) { generator=generator*1664525u+1013904223u; return (int)(generator>>16)-32768; }
static unsigned allocated(void) {
    /* The frozen 2.25 libc predates mallinfo2. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    struct mallinfo state=mallinfo();
#pragma GCC diagnostic pop
    return (unsigned)state.uordblks+(unsigned)state.hblkhd;
}

/* Compare the exact post-boundary samples in three independent instances.
 * The capture block sequence, lengths and every later input sample agree.
 * Only the older six-second prefix changes in the final instance. */
static int16_t common_output[600][160];
static unsigned common_packets, different_packets, last_difference;
static int16_t utterance[6*16000];

static int exercise(const char *mode, uint32_t seed) {
    generator=seed;
    invocations=0;
    alias_errors=0;
    bypass=!strcmp(mode,"bypass");
    int limited=!strncmp(mode,"four-",5);
    int speech=strstr(mode,"speech")!=NULL;
    int mapped=!strcmp(mode,"four-mapped") || !strcmp(mode,"four-speech");
    if(speech) {
        FILE *file=fopen("/speech.pcm","rb");
        if(!file || fread(utterance,sizeof(*utterance),6*16000,file)!=6*16000) return 2;
        fclose(file);
    }
    int continuity=!strncmp(mode,"history-",8);
    int record_common=!strcmp(mode,"history-first");
    common_packets=different_packets=last_difference=0;
    unsigned initial=allocated();
    void *aec[6];
    for(int c=0;c<6;c++)if(!(aec[c]=CreatAECHandler())) return 3;
    fprintf(stderr,"six AEC instances initialized\n");
    /* xaudio_create_doa at 0x169ec passes zero as its initial faulty-mic mask. */
    void *enh=CreatAFEEnhancementHandler("/usr/share/xiaomi/dnn_vad_model",
                                        !strcmp(mode,"four-masked") ? 3u : 0u);
    if(!enh) return 4;
    fprintf(stderr,"enhancement initialized\n");
    uint64_t hash=14695981039346656037ull, energy[2]={0};
    uint64_t mic_energy=0, clean_energy=0, second_energy=0;
    unsigned input_samples=0, other_samples=0;
    unsigned windows[2]={0}, valid=0;
    unsigned peak=allocated();
    clock_t start=clock();
    for (int serial=0;serial<(continuity ? 1500 : 750);++serial) {
        int f=serial%750;
        if(serial==750) generator=403u;
        int16_t ref[128], mic[6][128], cleaned[6][128], output[320]={0};
        for(int i=0;i<128;++i){
            int t=f*128+i, shared=noise()/96;
            int tone=f>=500 ? (int)(800*sin(t*(2*3.14159265*410/16000))) : 0;
            int reference=noise()/24;
            if(speech) reference=0;
            ref[i]=limited ? 0 : reference;
            for(int c=0;c<6;++c) {
                int captured=shared+noise()/32+tone+reference/2;
                if(speech) {
                    int offset=t-(c%3);
                    captured=offset>=0 ? utterance[offset] : 0;
                }
                /* Absent input and reference are zeroed before vendor code. */
                mic[c][i]=limited && c<2 ? 0 : captured;
            }
            if(mapped) {
                /* Offline virtual geometry. Slots 5 and 2 are nearest
                 * neighbours to missing slots 0 and 1 on the stock array. */
                mic[0][i]=mic[5][i];
                mic[1][i]=mic[2][i];
            }
        }
        for(int c=0;c<6;++c){
            int bytes=0;
            if(AECProcess(aec[c],mic[c],ref,1536,cleaned[c],&bytes)||bytes!=256) return 5;
        }
        if(mapped && f>=500) {
            for(unsigned i=0;i<128;i++) {
                mic_energy+=(int32_t)mic[2][i]*mic[2][i];
                clean_energy+=(int32_t)cleaned[2][i]*cleaned[2][i];
                ++input_samples;
            }
        }
        int bytes=0;float angle=0;
        AFEEnhancementProcess(enh,&cleaned[0][0],sizeof(cleaned),output,&bytes,0,&angle);
        unsigned current=allocated();
        if(current>peak) peak=current;
        if(!serial) fprintf(stderr,"first enhancement returned bytes=%d\n",bytes);
        if(!bytes)continue;
        if(bytes!=640) return 6;
        ++valid;
        if(continuity && serial>=750) {
            if(common_packets>=600) return 6;
            if(record_common) memcpy(common_output[common_packets],output,320);
            else if(memcmp(common_output[common_packets],output,320)) {
                ++different_packets;
                last_difference=common_packets+1;
            }
            ++common_packets;
        }
        for(unsigned i=0;i<160;++i) {
            if(mapped && f>=500) {
                second_energy+=(int32_t)output[i+160]*output[i+160];
                ++other_samples;
            }
            unsigned s=(uint16_t)output[i];
            hash=(hash^(s&255))*1099511628211ull;
            hash=(hash^(s>>8))*1099511628211ull;
            if(f>=250){
                unsigned group=f>=500;
                energy[group]+=(int32_t)output[i]*output[i];
                ++windows[group];
            }
        }
    }
    clock_t stop=clock();
    DestroyAFEEnhancementHandler(&enh);
    for(int c=0;c<6;++c)DestroyAECHandler(&aec[c]);
    if(mapped) printf("four-channel stages mic=%.2f aec=%.2f plane1=%.2f plane2=%.2f\n",
                     sqrt((double)mic_energy/input_samples),sqrt((double)clean_energy/input_samples),
                     sqrt((double)energy[1]/windows[1]),sqrt((double)second_energy/other_samples));
    fprintf(stderr,"resources audio_ms=%d emulated_cpu_ms=%.1f additional_heap_peak=%u after_destroy_delta=%ld\n",
        continuity ? 12000 : 6000,
        1000.*(stop-start)/CLOCKS_PER_SEC,
        peak-initial,(long)allocated()-(long)initial);
    if(continuity) {
        printf("%s common_packets=%u different=%u last_different=%u\n",
               mode,common_packets,different_packets,last_difference);
        if(common_packets!=600 || invocations!=1500 || alias_errors || valid!=1200) return 7;
        return (!strcmp(mode,"history-other") ? !different_packets : different_packets!=0);
    }
    printf("mode=%s calls=%d aliases=%d packets=%u noise_rms=%.2f near_mix_rms=%.2f hash=%016llx\n",
        mode,invocations,alias_errors,valid,sqrt((double)energy[0]/windows[0]),
        sqrt((double)energy[1]/windows[1]),(unsigned long long)hash);
    return invocations>0 && !alias_errors && valid>500 ? 0 : 7;
}

int main(int argc,char **argv) {
    if(argc!=2) return 2;
    Dl_info lib;
    if(!dladdr(dlsym(RTLD_DEFAULT,"AFEEnhancementProcess"),&lib)) return 8;
    vendor_base=(uintptr_t)lib.dli_fbase;
    struct sigaction action={.sa_sigaction=crash,.sa_flags=SA_SIGINFO};
    sigaction(SIGSEGV,&action,NULL);
    if(!strcmp(argv[1],"history")) {
        int result=exercise("history-first",42u);
        if(!result) result=exercise("history-same",42u);
        if(!result) result=exercise("history-other",69127u);
        return result;
    }
    if(!strcmp(argv[1],"recreate")) {
        int prior=exercise("old-source",69127u);
        if(prior) return prior;
        return exercise("new-source",42u);
    }
    if(!strcmp(argv[1],"four-fresh") || !strcmp(argv[1],"four-masked") ||
       !strcmp(argv[1],"four-mapped") || !strcmp(argv[1],"six-speech") ||
       !strcmp(argv[1],"four-speech")) return exercise(argv[1],42u);
    if(!strcmp(argv[1],"four-recreate")) {
        int prior=exercise("four-old",69127u);
        if(prior) return prior;
        return exercise("four-new",42u);
    }
    if(strcmp(argv[1],"bypass") && strcmp(argv[1],"enabled")) return 2;
    return exercise(argv[1],42u);
}
