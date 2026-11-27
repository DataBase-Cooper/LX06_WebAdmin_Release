#include "media.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#define TAU 6.283185307179586476925286766559
#define RAMP_SAMPLES 80u /* 5 ms attack/release avoids hard-edge clicks */
dc_media_cue_config dc_media_cue_defaults(void){return (dc_media_cue_config){20,1000,3000,440,480,-30.0};}
static int valid(const dc_media_cue_config*c){return (c->frame_ms==10||c->frame_ms==20)&&c->on_ms>=50&&c->on_ms<=10000&&c->off_ms<=30000&&c->frequency_a>=80&&c->frequency_a<=3500&&c->frequency_b>=80&&c->frequency_b<=3500&&c->frequency_a!=c->frequency_b&&isfinite(c->level_db)&&c->level_db>=-60&&c->level_db<=-18;}
int dc_media_cue_begin(dc_media_cue*c,uint64_t epoch,const dc_media_cue_config*config){
    if(!c||!epoch)return -EINVAL;
    dc_media_cue_config use=config?*config:dc_media_cue_defaults();
    if(!valid(&use))return -EINVAL;
    if(c->active)return -EBUSY;
    memset(c,0,sizeof *c);c->config=use;c->tag=(dc_tag){epoch,DC_MEDIA_CUE_GENERATION,0};
    c->on_samples=use.on_ms*16u;c->cycle_samples=(use.on_ms+use.off_ms)*16u;
    c->amplitude=32767.0*pow(10.0,use.level_db/20.0)/2.0;
    double a=TAU*use.frequency_a/DC_MEDIA_RATE,b=TAU*use.frequency_b/DC_MEDIA_RATE;
    c->step_sine_a=sin(a);c->step_cosine_a=cos(a);c->step_sine_b=sin(b);c->step_cosine_b=cos(b);
    c->cosine_a=c->cosine_b=1;c->active=1;return 0;
}
int dc_media_cue_next(dc_media_cue*c,size_t queued,dc_media_frame*out){
    if(!c||!out)return -EINVAL;
    if(!c->active)return 0;
    unsigned samples=c->config.frame_ms*16u;
    if(queued>DC_MEDIA_CUE_PREFILL_SAMPLES||samples>DC_MEDIA_CUE_PREFILL_SAMPLES-queued)return -EAGAIN;
    memset(out,0,sizeof *out);out->tag=c->tag;out->role=DC_MEDIA_CONNECTING;out->rate=DC_MEDIA_RATE;out->samples=samples;
    for(unsigned i=0;i<samples;i++){
        if(c->position<c->on_samples){
            if(!c->position){c->sine_a=c->sine_b=0;c->cosine_a=c->cosine_b=1;}
            double envelope=1.0;
            if(c->position<RAMP_SAMPLES)envelope=(double)c->position/RAMP_SAMPLES;
            else if(c->on_samples-c->position<RAMP_SAMPLES)envelope=(double)(c->on_samples-c->position)/RAMP_SAMPLES;
            out->pcm[i]=(int16_t)lrint((c->sine_a+c->sine_b)*c->amplitude*envelope);
            double a=c->sine_a*c->step_cosine_a+c->cosine_a*c->step_sine_a;
            c->cosine_a=c->cosine_a*c->step_cosine_a-c->sine_a*c->step_sine_a;c->sine_a=a;
            double b=c->sine_b*c->step_cosine_b+c->cosine_b*c->step_sine_b;
            c->cosine_b=c->cosine_b*c->step_cosine_b-c->sine_b*c->step_sine_b;c->sine_b=b;
        }
        if(++c->position==c->cycle_samples)c->position=0;
    }
    return 1;
}
dc_tag dc_media_cue_stop(dc_media_cue*c){dc_tag tag={0};if(c){if(c->active)tag=c->tag;memset(c,0,sizeof *c);}return tag;}
int dc_media_cue_active(const dc_media_cue*c){return c&&c->active;}
