#include "audio_route.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct turn {dc_tag tag;uint64_t end;unsigned committed,cancelled,bound,greeting,local;};
struct dc_audio_route {
 uint64_t epoch,read,write,speech_start,speech_utterance,accepted_utterance;
 uint64_t sequence,highest_sequence,cancel_since,token,next_token;
 dc_tag cloud_tag;
 unsigned ready,barge_any,speech,prefix_lost,cloud,cloud_end;
 unsigned granted,cancelled,cancel_sent,turns;
 struct turn turn[DC_AUDIO_TURNS];
 dc_audio_action offer;
 unsigned char mic[DC_AUDIO_MIC_CAP];
};
static int tag_valid(const dc_audio_route *r,dc_tag tag){return tag.session_epoch==r->epoch&&tag.reply_generation&&tag.utterance_id;}
static void changed(dc_audio_route *r){r->token=0;}
static int find(const dc_audio_route *r,dc_tag tag){for(unsigned i=0;i<r->turns;i++)if(dc_tag_equal(r->turn[i].tag,tag))return (int)i;return -1;}
static void remove_turn(dc_audio_route *r,unsigned at){
 if(at+1<r->turns)memmove(r->turn+at,r->turn+at+1,(r->turns-at-1)*sizeof(*r->turn));
 r->turns--;memset(r->turn+r->turns,0,sizeof(*r->turn));
}
static int protected(const dc_audio_route *r){return r->cloud||r->turns;}
static int barrier(const dc_audio_route *r){
 if(r->cloud&&r->cancelled)return 1;
 for(unsigned i=0;i<r->turns;i++)if(r->turn[i].cancelled&&r->turn[i].committed&&!r->turn[i].bound)return 1;
 return 0;
}
static int uncommitted(const dc_audio_route *r){
 for(unsigned i=0;i<r->turns;i++)if(!r->turn[i].committed&&!r->turn[i].bound)return (int)i;
 return -1;
}
static void trim(dc_audio_route *r,uint64_t to){if(to>r->read)r->read=to<r->write?to:r->write;}
static void unpin(dc_audio_route *r){r->speech=0;r->speech_utterance=0;r->prefix_lost=0;}
static void clear_cloud(dc_audio_route *r){
 int at=find(r,r->cloud_tag);if(at>=0)remove_turn(r,(unsigned)at);
 r->cloud=0;r->cloud_end=0;r->granted=0;r->cancelled=0;r->cancel_sent=0;
 r->cancel_since=0;r->sequence=0;memset(&r->cloud_tag,0,sizeof(r->cloud_tag));
}
dc_audio_route *dc_audio_route_create(uint64_t epoch,unsigned barge_any){
 if(!epoch||barge_any>1)return NULL;
 dc_audio_route *r=calloc(1,sizeof(*r));if(r){r->epoch=epoch;r->barge_any=barge_any;}return r;
}
void dc_audio_route_destroy(dc_audio_route *r){if(r){memset(r,0,sizeof(*r));free(r);}}
int dc_audio_route_ready(dc_audio_route *r){if(!r)return -EINVAL;changed(r);r->ready=1;return 0;}
int dc_audio_route_greeting(dc_audio_route *r,dc_tag tag,uint64_t now){
 (void)now;
 if(!r||!tag_valid(r,tag)||!r->ready)return -EINVAL;
 if(r->turns||r->cloud||r->highest_sequence)return -EALREADY;
 changed(r);r->turn[r->turns++]=(struct turn){.tag=tag,.end=r->read,.committed=1,.greeting=1};return 0;
}
int dc_audio_route_local_greeting(dc_audio_route *r, dc_tag tag, uint64_t now) {
    int result=dc_audio_route_greeting(r,tag,now);
    if(!result)r->turn[0].bound=r->turn[0].local=1;
    return result;
}
void dc_audio_route_local_greeting_done(dc_audio_route *r,dc_tag tag) {
    int at=r?find(r,tag):-1;
    if(at<0||!r->turn[at].local)return;
    changed(r);
    remove_turn(r,(unsigned)at);
    if(!r->speech)trim(r,r->write);
}
int dc_audio_route_capture(dc_audio_route *r,const void *data,size_t bytes,uint64_t utterance,int near,uint64_t now){
 (void)now;
 if(!r||(!data&&bytes)||!bytes||bytes%2||bytes>DC_AUDIO_MIC_CAP||UINT64_MAX-r->write<bytes)return -EINVAL;
 changed(r);
 if(near&&(!r->speech||(utterance&&r->speech_utterance&&utterance!=r->speech_utterance))){
  r->speech=1;r->speech_utterance=utterance;r->prefix_lost=0;
  r->speech_start=r->write>DC_AUDIO_PRE_BYTES?r->write-DC_AUDIO_PRE_BYTES:0;
  if(r->speech_start<r->read)r->speech_start=r->read;
  if(protected(r)&&!r->barge_any&&uncommitted(r)<0&&!barrier(r))trim(r,r->speech_start);
 }else if(r->speech&&utterance&&!r->speech_utterance)r->speech_utterance=utterance;
 if(protected(r)&&(!r->barge_any||(r->turns&&r->turn[0].local))&&
    uncommitted(r)<0&&!barrier(r)&&!r->speech){
  uint64_t to=r->write+bytes>DC_AUDIO_PRE_BYTES?r->write+bytes-DC_AUDIO_PRE_BYTES:0;trim(r,to);
 }
 if(bytes>DC_AUDIO_MIC_CAP-(size_t)(r->write-r->read)){
  if(barrier(r)||uncommitted(r)>=0||!protected(r)||r->barge_any)return -ENOBUFS;
  uint64_t to=r->write+bytes-DC_AUDIO_MIC_CAP;
  if(r->speech&&to>r->speech_start)r->prefix_lost=1;
  trim(r,to);
 }
 size_t at=(size_t)(r->write%DC_AUDIO_MIC_CAP),first=DC_AUDIO_MIC_CAP-at;if(first>bytes)first=bytes;
 memcpy(r->mic+at,data,first);if(bytes>first)memcpy(r->mic,(const unsigned char *)data+first,bytes-first);
 r->write+=bytes;return 0;
}
int dc_audio_route_question(dc_audio_route *r,dc_tag tag,uint64_t now){
 (void)now;
 if(!r||!tag_valid(r,tag))return -EINVAL;
 changed(r);int at=find(r,tag);if(at>=0)return 0;
 if(r->prefix_lost)return -ENOBUFS;
 if(r->turns==DC_AUDIO_TURNS)return -ENOSPC;
 r->accepted_utterance=tag.utterance_id;
 r->turn[r->turns++]=(struct turn){.tag=tag,.end=r->write};
 return 0;
}
int dc_audio_route_final(dc_audio_route *r,uint64_t utterance){
 if(!r||!utterance)return -EINVAL;
 changed(r);
 if(utterance==r->accepted_utterance)return 0;
 if(r->speech&&(!r->speech_utterance||r->speech_utterance==utterance)&&!barrier(r)&&uncommitted(r)<0){
  unpin(r);
  if(protected(r)&&!r->barge_any)trim(r,r->write>DC_AUDIO_PRE_BYTES?r->write-DC_AUDIO_PRE_BYTES:0);
 }
 return 0;
}
int dc_audio_route_cancel(dc_audio_route *r,dc_tag tag,uint64_t now){
 if(!r||!tag_valid(r,tag))return -EINVAL;
 changed(r);int at=find(r,tag);
 if(r->cloud&&dc_tag_equal(tag,r->cloud_tag)){
  if(!r->cancelled){r->cancelled=1;r->cancel_since=now;if(r->speech)trim(r,r->speech_start);}
  if(at>=0)r->turn[at].cancelled=1;
  return 0;
 }
 if(at<0)return 0;
 struct turn *t=r->turn+at;
 if(t->cancelled)return 0;
 /* PCM may already have reached a server with its own VAD before its explicit
  * commit. Retain even an unsent commit as a tombstone; otherwise a late BEGIN
  * could be assigned to the next user question. Finish that fixed watermark,
  * commit it silently, then cancel its BEGIN without ever granting playback. */
 t->cancelled=1;if(!r->cancel_since)r->cancel_since=now;
 return 0;
}
int dc_audio_route_begin(dc_audio_route *r,uint64_t seq,uint64_t now,dc_tag *tag){
 (void)now;
 if(!r||!seq||!tag)return -EINVAL;
 memset(tag,0,sizeof(*tag));
 if(seq<=r->highest_sequence)return DC_AUDIO_BEGIN_STALE;
 if(r->cloud)return -EPROTO;
 changed(r);r->highest_sequence=seq;r->sequence=seq;r->cloud=1;
 for(unsigned i=0;i<r->turns;i++)if(r->turn[i].committed&&!r->turn[i].bound){
  r->turn[i].bound=1;r->cloud_tag=r->turn[i].tag;*tag=r->cloud_tag;
  if(r->turn[i].cancelled){r->cancelled=1;return DC_AUDIO_BEGIN_CANCELLED;}
  return DC_AUDIO_BEGIN_KNOWN;
 }
 for(unsigned i=0;i<r->turns;i++)if(!r->turn[i].committed&&!r->turn[i].bound){
  /* A native server can endpoint before our explicit commit. It is only safe
   * to attach the already complete local question if every byte through that
   * question's FINAL watermark has actually crossed the adapter boundary. */
  if(r->read<r->turn[i].end)return -EPROTO;
  r->turn[i].committed=r->turn[i].bound=1;r->cloud_tag=r->turn[i].tag;*tag=r->cloud_tag;
  if(r->turn[i].cancelled){r->cancelled=1;return DC_AUDIO_BEGIN_CANCELLED;}
  return DC_AUDIO_BEGIN_KNOWN;
 }
 return DC_AUDIO_BEGIN_NEED_TAG;
}
int dc_audio_route_bind(dc_audio_route *r,uint64_t seq,dc_tag tag,uint64_t now){
 (void)now;
 if(!r||!tag_valid(r,tag)||!r->cloud||r->sequence!=seq||r->cloud_tag.session_epoch)return -EINVAL;
 changed(r);r->cloud_tag=tag;unpin(r);return 0;
}
int dc_audio_route_end(dc_audio_route *r,uint64_t seq,uint64_t generation){
 if(!r)return -EINVAL;
 if(!dc_audio_route_accepts(r,seq,generation))return 0;
 changed(r);r->cloud_end=1;return 1;
}
int dc_audio_route_played(dc_audio_route *r,dc_tag tag){
 if(!r||!tag_valid(r,tag))return -EINVAL;
 if(!r->cloud||!dc_tag_equal(tag,r->cloud_tag)||r->cancelled)return 0;
 if(!r->cloud_end)return -EAGAIN;
 changed(r);clear_cloud(r);
 /* Never clear the ring here: a real user may already be speaking over the
  * final syllable. Keep its near-end prefix; discard only unpinned echo tail. */
 if(!r->speech)trim(r,r->write);
 return 1;
}
int dc_audio_route_resumed(dc_audio_route *r,uint64_t seq,uint64_t generation){
 if(!r)return -EINVAL;
 if(!r->cloud||!r->cancelled||!r->cancel_sent||r->sequence!=seq||r->cloud_tag.reply_generation!=generation)return 0;
 changed(r);clear_cloud(r);return 1;
}
int dc_audio_route_accepts(const dc_audio_route *r,uint64_t seq,uint64_t generation){
 return r&&r->cloud&&r->granted&&!r->cancelled&&r->sequence==seq&&r->cloud_tag.reply_generation==generation;
}
static int offer(dc_audio_route *r,dc_audio_action *out,unsigned type,dc_tag tag,uint64_t seq,const unsigned char *pcm,size_t bytes){
 r->offer=(dc_audio_action){.type=type,.tag=tag,.sequence=seq,.token=++r->next_token,.pcm=pcm,.bytes=bytes};
 if(!r->offer.token)r->offer.token=++r->next_token;
 r->token=r->offer.token;*out=r->offer;return 1;
}
int dc_audio_route_next(dc_audio_route *r,dc_audio_action *out){
 if(!r||!out)return -EINVAL;
 r->token=0;memset(out,0,sizeof(*out));if(!r->ready)return 0;
 if(r->cloud&&r->cancelled&&!r->cancel_sent)return offer(r,out,DC_AUDIO_CANCEL,r->cloud_tag,r->sequence,NULL,0);
 if(r->cloud&&r->cloud_tag.session_epoch&&!r->cancelled&&!r->granted)return offer(r,out,DC_AUDIO_GRANT,r->cloud_tag,r->sequence,NULL,0);
 if(barrier(r))return 0;
 /* Do not let native server VAD allocate an unsolicited first reply before
  * the explicit greeting's BEGIN has bound its separate core generation. */
 for(unsigned i=0;i<r->turns;i++)if(r->turn[i].greeting&&(!r->turn[i].bound||r->turn[i].local))return 0;
 int at=uncommitted(r);
 if(r->prefix_lost&&(!protected(r)||at>=0))return -ENOBUFS;
 if(at<0&&protected(r)&&!r->barge_any)return 0;
 uint64_t limit=at>=0?r->turn[at].end:r->write;
 if(r->read<limit){
  size_t position=(size_t)(r->read%DC_AUDIO_MIC_CAP),bytes=(size_t)(limit-r->read);
  if(bytes>960)bytes=960;
  if(bytes>DC_AUDIO_MIC_CAP-position)bytes=DC_AUDIO_MIC_CAP-position;
  dc_tag none={.session_epoch=r->epoch};return offer(r,out,DC_AUDIO_MIC,none,0,r->mic+position,bytes);
 }
 if(at>=0)return offer(r,out,DC_AUDIO_COMMIT,r->turn[at].tag,0,NULL,0);
 return 0;
}
int dc_audio_route_sent(dc_audio_route *r,const dc_audio_action *a,uint64_t now){
 (void)now;
 if(!r||!a||!r->token||a->token!=r->token||a->type!=r->offer.type||!dc_tag_equal(a->tag,r->offer.tag)||a->sequence!=r->offer.sequence||a->bytes!=r->offer.bytes||a->pcm!=r->offer.pcm)return -ESTALE;
 changed(r);
 switch(a->type){
 case DC_AUDIO_MIC:r->read+=a->bytes;return 0;
 case DC_AUDIO_COMMIT:{int at=find(r,a->tag);if(at<0)return -EPROTO;r->turn[at].committed=1;unpin(r);return 0;}
 case DC_AUDIO_GRANT:r->granted=1;return 0;
 case DC_AUDIO_CANCEL:r->cancel_sent=1;return 0;
 default:return -EINVAL;
 }
}
int dc_audio_route_tick(dc_audio_route *r,uint64_t now){
 if(!r)return -EINVAL;
 return barrier(r)&&r->cancel_since&&now>=r->cancel_since&&now-r->cancel_since>15000?-ETIMEDOUT:0;
}
void dc_audio_route_status(const dc_audio_route *r,dc_audio_status *out){
 if(!r||!out)return;
 memset(out,0,sizeof(*out));out->cloud_tag=r->cloud_tag;out->cloud_sequence=r->sequence;out->highest_sequence=r->highest_sequence;
 out->captured_bytes=r->write;out->sent_bytes=r->read;out->microphone_bytes=(size_t)(r->write-r->read);
 out->turns=r->turns;out->ready=r->ready;out->protected_output=protected(r);out->cancel_barrier=barrier(r);out->prefix_lost=r->prefix_lost;
 for(unsigned i=0;i<r->turns;i++)if(!r->turn[i].bound){out->pending_tag=r->turn[i].tag;break;}
}
