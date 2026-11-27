#include "../media.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PI 3.14159265358979323846
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"media-fixture-failed line=%u\n",__LINE__);exit(1);}}while(0)
static double magnitude(const int16_t*pcm,unsigned count,unsigned frequency){double re=0,im=0;for(unsigned i=0;i<count;i++){double angle=2*PI*frequency*i/16000;re+=pcm[i]*cos(angle);im+=pcm[i]*sin(angle);}return sqrt(re*re+im*im);}
int main(void){
    dc_media_cue cue={0};dc_media_frame f;dc_media_cue_config config=dc_media_cue_defaults();
    CHECK(!dc_media_cue_active(&cue));CHECK(dc_media_cue_next(&cue,0,&f)==0);CHECK(!dc_media_cue_begin(&cue,17,NULL));CHECK(dc_media_cue_active(&cue));
    CHECK(dc_media_cue_begin(&cue,18,NULL)==-EBUSY);
    int16_t ring[16000];unsigned peak=0,at=0;
    for(unsigned frame=0;frame<200;frame++){
        CHECK(dc_media_cue_next(&cue,0,&f)==1);CHECK(f.role==DC_MEDIA_CONNECTING&&f.rate==16000&&f.samples==320);
        CHECK(f.tag.session_epoch==17&&f.tag.reply_generation==UINT64_MAX&&f.tag.utterance_id==0);
        for(unsigned i=0;i<f.samples;i++){unsigned amplitude=(unsigned)abs(f.pcm[i]);if(amplitude>peak)peak=amplitude;if(frame<50)ring[at++]=f.pcm[i];else CHECK(!f.pcm[i]);}
    }
    CHECK(at==16000&&peak>400&&peak<=1037);CHECK(ring[0]==0);
    CHECK(magnitude(ring,16000,440)>20*magnitude(ring,16000,1000));CHECK(magnitude(ring,16000,480)>20*magnitude(ring,16000,1000));
    CHECK(dc_media_cue_next(&cue,0,&f)==1);CHECK(!memcmp(ring,f.pcm,640));
    dc_tag old=dc_media_cue_stop(&cue);CHECK(old.session_epoch==17&&old.reply_generation==UINT64_MAX&&!old.utterance_id);CHECK(!dc_media_cue_active(&cue));CHECK(dc_media_cue_next(&cue,0,&f)==0);CHECK(!dc_media_cue_stop(&cue).session_epoch);
    config.frame_ms=10;config.on_ms=55;config.off_ms=105;CHECK(!dc_media_cue_begin(&cue,18,&config));
    dc_media_cue before=cue;CHECK(dc_media_cue_next(&cue,DC_MEDIA_CUE_PREFILL_SAMPLES,&f)==-EAGAIN);CHECK(!memcmp(&cue,&before,sizeof cue));CHECK(dc_media_cue_next(&cue,SIZE_MAX,&f)==-EAGAIN);CHECK(!memcmp(&cue,&before,sizeof cue));
    CHECK(dc_media_cue_next(&cue,DC_MEDIA_CUE_PREFILL_SAMPLES-160,&f)==1);CHECK(f.samples==160&&f.tag.session_epoch==18);
    for(unsigned frame=1;frame<32;frame++){CHECK(dc_media_cue_next(&cue,0,&f)==1);for(unsigned i=0;i<160;i++){unsigned position=(frame*160+i)%2560;if(position>=880)CHECK(!f.pcm[i]);}}
    dc_media_cue_stop(&cue);
    CHECK(dc_media_cue_begin(NULL,1,NULL)==-EINVAL);CHECK(dc_media_cue_begin(&cue,0,NULL)==-EINVAL);CHECK(dc_media_cue_next(NULL,0,&f)==-EINVAL);CHECK(dc_media_cue_next(&cue,0,NULL)==-EINVAL);
    dc_media_cue_config bad=config;bad.frame_ms=30;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);bad=config;bad.level_db=NAN;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);bad=config;bad.level_db=-17;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);bad=config;bad.frequency_a=bad.frequency_b;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);bad=config;bad.on_ms=0;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);bad=config;bad.off_ms=30001;CHECK(dc_media_cue_begin(&cue,1,&bad)==-EINVAL);
    CHECK(!dc_media_cue_active(&cue));CHECK(DC_MEDIA_GREETING!=DC_MEDIA_REPLY&&DC_MEDIA_ACTION_RESULT!=DC_MEDIA_CONNECTING);
    printf("{\"passed\":true,\"assertions\":%u,\"cue_bytes\":%zu,\"frame_bytes\":%zu}\n",checks,sizeof cue,sizeof f);return 0;
}
