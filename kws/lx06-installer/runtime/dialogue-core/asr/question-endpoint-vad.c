/* Independent research module. No socket, file, SDK, player, logging or thread.
 * With raw PCM, qev_feed(...,NULL,...) refuses VAD endpoint evidence. It does
 * not replace the original endpoint until a real framed producer is reviewed.
 */
#include "question-endpoint-vad.h"
#include <fvad.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define QEV_RATE 16000U
#define QEV_STABLE_MS 1600U
#define QEV_TAIL_MS 300U
#define QEV_FRESH_MS 120U
#define QEV_MAX_PRODUCER_GAP_MS 100U

struct qev_endpoint {
 Fvad *vad;
 struct qev_stamp previous;
 uint64_t turn,turn_start,nonvoice_first,nonvoice_first_samples;
 uint64_t nonvoice_samples,last_read,real_vad_calls,valid_live_frames;
 unsigned voice_seen,metadata_valid,frame_code;
};
static void evidence_clear(qev_endpoint *q,unsigned code){
 memset(&q->previous,0,sizeof(q->previous));q->voice_seen=q->metadata_valid=0;
 q->nonvoice_first=q->nonvoice_first_samples=q->nonvoice_samples=0;q->frame_code=code;
}
qev_endpoint *qev_new(unsigned profile){
 if(profile!=1)return NULL;
 qev_endpoint *q=calloc(1,sizeof(*q));if(!q)return NULL;
 q->vad=fvad_new();
 if(!q->vad||fvad_set_sample_rate(q->vad,QEV_RATE)||fvad_set_mode(q->vad,0)){
  if(q->vad)fvad_free(q->vad);
  free(q);return NULL;
 }
 return q;
}
void qev_free(qev_endpoint *q){if(q){if(q->vad)fvad_free(q->vad);memset(q,0,sizeof(*q));free(q);}}
void qev_begin(qev_endpoint *q,uint64_t turn,uint64_t started){
 if(!q)return;
 q->turn=turn;q->turn_start=started;q->last_read=0;
 evidence_clear(q,QEV_NO_METADATA);
 /* Acoustic noise adaptation may survive turns; endpoint evidence never does. */
}
static int reject(qev_endpoint*q,unsigned code){evidence_clear(q,code);return -1;}
static int frame_validate(qev_endpoint*q,size_t samples,const struct qev_stamp*s,uint64_t now){
 if(!q||!q->turn)return -1;
 if(!s)return reject(q,QEV_NO_METADATA);
 if(s->origin!=QEV_LIVE)return reject(q,QEV_NOT_LIVE);
 if(s->rate!=QEV_RATE||s->samples!=samples||
    (samples!=160&&samples!=320&&samples!=480)||!s->producer_pid||!s->producer_birth_ticks)
  return reject(q,QEV_BAD_FORMAT);
 if(!s->available_mono_ms||s->available_mono_ms<q->turn_start||
    now<s->available_mono_ms||(q->last_read&&now<q->last_read))return reject(q,QEV_BAD_TIME);
 if(now-s->available_mono_ms>QEV_FRESH_MS)return reject(q,QEV_STALE);
 unsigned continuous=q->metadata_valid;q->frame_code=QEV_FRAME_OK;
 if(continuous&&(s->producer_pid!=q->previous.producer_pid||
    s->producer_birth_ticks!=q->previous.producer_birth_ticks||
    q->previous.sequence==UINT64_MAX||s->sequence!=q->previous.sequence+1||
    s->available_mono_ms<=q->previous.available_mono_ms||
    s->available_mono_ms-q->previous.available_mono_ms>QEV_MAX_PRODUCER_GAP_MS)){
  evidence_clear(q,QEV_DISCONTINUITY);continuous=0;
 }
 q->previous=*s;q->last_read=now;q->metadata_valid=1;
 /* A discontinuity's first good frame starts fresh evidence, not old elapsed time. */
 return continuous?1:0;
}
static int frame_decision(qev_endpoint*q,size_t samples,const struct qev_stamp*s,int decision){
 if(decision<0||decision>1)return reject(q,QEV_CLASSIFIER_ERROR);
 q->valid_live_frames++;
 if(decision){q->voice_seen=1;q->nonvoice_samples=0;q->nonvoice_first=0;q->nonvoice_first_samples=0;}
 else{
  if(!q->nonvoice_samples){q->nonvoice_first=s->available_mono_ms;q->nonvoice_first_samples=samples;}
  if(q->nonvoice_samples>UINT64_MAX-samples)return reject(q,QEV_BAD_FORMAT);
  q->nonvoice_samples+=samples;
 }
 return decision;
}
int qev_feed(qev_endpoint*q,const int16_t*pcm,size_t samples,const struct qev_stamp*s,uint64_t now){
 if(!q)return -1;
 if(frame_validate(q,samples,s,now)<0)return -1;
 if(!pcm)return reject(q,QEV_BAD_FORMAT);
 q->real_vad_calls++;
 return frame_decision(q,samples,s,fvad_process(q->vad,pcm,samples));
}
static uint64_t nonvoice_span(const qev_endpoint*q){
 if(!q->nonvoice_samples||q->previous.available_mono_ms<q->nonvoice_first)return 0;
 uint64_t span=q->previous.available_mono_ms-q->nonvoice_first;
 uint64_t first_ms=q->nonvoice_first_samples/(QEV_RATE/1000U);
 return span>UINT64_MAX-first_ms?UINT64_MAX:span+first_ms;
}
int qev_ready(const qev_endpoint*q,const struct qev_gate*g,uint64_t now){
 if(!q||!g||!q->turn||g->turn!=q->turn||!g->active||!g->protect||!g->partial_nonempty||
    g->ended||g->held||g->replaying||g->pending_bytes||g->outbox_bytes||g->partial_pcm_bytes||
    !g->last_partial_mono_ms||g->last_partial_mono_ms<q->turn_start||
    now<g->last_partial_mono_ms||
    now-g->last_partial_mono_ms<QEV_STABLE_MS||!q->voice_seen||!q->metadata_valid||
    now<q->last_read||now<q->previous.available_mono_ms||
    now-q->previous.available_mono_ms>QEV_FRESH_MS||
    q->nonvoice_samples<(QEV_RATE/1000U)*QEV_TAIL_MS||nonvoice_span(q)<QEV_TAIL_MS)return 0;
 return 1;
}
void qev_stats_read(const qev_endpoint*q,struct qev_stats*out){
 if(!out)return;
 memset(out,0,sizeof(*out));if(!q)return;
 out->turn=q->turn;out->last_available_mono_ms=q->previous.available_mono_ms;
 out->nonvoice_samples=q->nonvoice_samples;out->nonvoice_span_ms=nonvoice_span(q);
 out->real_vad_calls=q->real_vad_calls;out->valid_live_frames=q->valid_live_frames;
 out->voice_seen=q->voice_seen;out->frame_code=q->frame_code;out->metadata_valid=q->metadata_valid;
}
#ifdef QEV_HOST_TEST
int qev_feed_decision_for_test(qev_endpoint*q,size_t samples,const struct qev_stamp*s,uint64_t now,int decision){
 if(!q||frame_validate(q,samples,s,now)<0)return -1;
 return frame_decision(q,samples,s,decision);
}
#endif
