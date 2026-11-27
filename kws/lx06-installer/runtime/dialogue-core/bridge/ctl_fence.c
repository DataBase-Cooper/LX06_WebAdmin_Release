#include "ctl_fence.h"
#include <string.h>
_Static_assert(sizeof(br_ctl_fence)<256,"bounded CTL fence");
static int identity(const br_ctl_fence *s,uint64_t run,uint64_t cycle){
    return s&&!s->busy&&run&&run==s->run&&cycle==s->cycle&&
        s->phase!=BR_FENCE_OFF&&s->phase!=BR_FENCE_CLOSED;
}
static int close_gate(br_ctl_fence *s,const char *error){
    s->phase=BR_FENCE_CLOSED;s->error=error;s->confirmed_audio=s->next_audio=0;
    s->next_generation=0;s->acked=0;
    /* Mark closed BEFORE callbacks: teardown cannot reopen this gate. */
    s->busy=1;
    (void)br_ctl_disconnect(s->ctl,s->run,s->epoch,s->now_ms);
    s->busy=0;return -1;
}
static int current(br_ctl_fence *s,uint64_t now){
    br_ctl *c=s->ctl;
    if(!c||c->run!=s->run||c->epoch!=s->epoch)return close_gate(s,"bridge_unreachable");
    if(now<s->now_ms||now<c->now_ms)return close_gate(s,"bridge_unreachable");
    s->now_ms=now;
    if(!c->caps||!c->caps->ready||c->caps->connection!=s->epoch||
       c->phase==BR_CTL_IDLE||c->phase==BR_CTL_DRAINING||c->phase==BR_CTL_CLOSED||
       (c->cycle!=s->cycle&&!(s->phase==BR_FENCE_ACTIVE&&c->phase==BR_CTL_QUIET)))
        return close_gate(s,"bridge_unreachable");
    if(s->phase==BR_FENCE_RESUMING&&now-s->resume_ms>=6000)return close_gate(s,"op_timeout");
    return 1;
}
static int control_payload(void *p,const dc_wire_header *h,const unsigned char *data,size_t n){
    (void)p;(void)data;
    return !n&&!h->flags&&h->generation&&h->sequence&&
        (h->type==BR_WIRE_CANCEL||h->type==BR_WIRE_RESUME||
         h->type==BR_WIRE_CANCEL_ACK||h->type==BR_WIRE_RESUMED||h->type==BR_WIRE_REPLY_END);
}
static int control(br_ctl_fence *s,unsigned type,uint64_t gen,uint64_t seq,
                   dc_wire_packet *p,void *out,size_t cap,size_t *length){
    if(!p)return -1;
    memset(p,0,sizeof(*p));
    p->header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=LX06_BRIDGE_WIRE_VERSION,
        .type=(uint16_t)type,.epoch=s->epoch,.generation=gen,.sequence=seq};
    int rc=br_wire_encode(p,BR_FROM_SPEAKER,s->epoch,control_payload,NULL,out,cap,length);
    memset(p,0,sizeof(*p));return rc==BR_WIRE_OK?1:-1;
}
void br_ctl_fence_init(br_ctl_fence *s){if(s)memset(s,0,sizeof(*s));}
int br_ctl_fence_begin(br_ctl_fence *s,br_ctl *c,uint64_t run,uint64_t gen,
                       unsigned audio,uint64_t now,br_ctl_turn_released released,void *context){
    if(!s||s->busy||(s->phase!=BR_FENCE_OFF&&s->phase!=BR_FENCE_CLOSED)||!c||
       !run||run<=s->last_run||run!=c->run||!c->epoch||c->epoch==s->last_epoch||!gen||audio>7||
       c->phase!=BR_CTL_ACTIVE||!c->config.declared||!c->caps||!c->caps->ready||
       c->caps->connection!=c->epoch||now<c->now_ms||(s->last_run&&now<s->now_ms)||!released)return -1;
    s->ctl=c;s->released=released;s->context=context;s->run=s->last_run=run;
    s->epoch=s->last_epoch=c->epoch;s->generation=gen;s->next_generation=0;
    s->cycle=c->cycle;s->confirmed_audio=audio;s->next_audio=0;s->now_ms=now;
    s->audio_sequence=s->mic_sequence=0;s->cancel_sequence=s->cancel_generation=0;s->acked=s->reply_ended=0;
    s->phase=BR_FENCE_ACTIVE;s->error=NULL;return 1;
}
int br_ctl_fence_cancel(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t sequence,
                        uint64_t now,dc_wire_packet *p,void *wire,size_t capacity,size_t *length){
    /* New cycle is not installed until encoding succeeds; stale cycles cannot
     * advance the live clock. The CTL quiesce has already gated local IO. */
    if(!s||s->busy||!run||run!=s->run||!cycle||cycle<=s->last_cycle||
       s->phase!=BR_FENCE_ACTIVE||!sequence||sequence<=s->last_cancel||!s->ctl||
       s->ctl->run!=run||s->ctl->epoch!=s->epoch||s->ctl->phase!=BR_CTL_QUIET||s->ctl->cycle!=cycle)return -1;
    if(current(s,now)!=1)return -1;
    if(control(s,BR_WIRE_CANCEL,s->generation,sequence,p,wire,capacity,length)!=1)return -1;
    s->cycle=s->last_cycle=cycle;s->cancel_sequence=s->last_cancel=sequence;
    s->cancel_generation=s->generation;
    s->phase=BR_FENCE_FROZEN;s->acked=0;return 1;
}
int br_ctl_fence_resume(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t gen,
                        unsigned audio,uint64_t now,dc_wire_packet *p,void *wire,size_t capacity,size_t *length){
    if(!identity(s,run,cycle))return -1;
    if(current(s,now)!=1||s->phase!=BR_FENCE_FROZEN||s->ctl->phase!=BR_CTL_RESUMING||
       !s->acked||gen<=s->generation||audio>7||(audio&~s->confirmed_audio))return -1;
    if(control(s,BR_WIRE_RESUME,gen,s->cancel_sequence,p,wire,capacity,length)!=1)return -1;
    s->next_generation=gen;s->next_audio=audio;s->resume_ms=now;s->phase=BR_FENCE_RESUMING;return 1;
}
int br_ctl_fence_receive(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t now,
                         const void *data,size_t n,dc_wire_packet *p){
    int rc=-1;
    if(!p)return -1;
    memset(p,0,sizeof(*p));
    if(!identity(s,run,cycle))return -1;
    if(current(s,now)!=1)return -1;
    int decoded=br_wire_decode(data,n,BR_FROM_PHONE,s->epoch,control_payload,NULL,p);
    if(decoded!=BR_WIRE_OK){close_gate(s,decoded==BR_WIRE_VERSION?"bridge_version_mismatch":"unsupported");goto done;}
    const dc_wire_header *h=&p->header;
    if(h->type==BR_WIRE_AUDIO){
        if(!h->generation){close_gate(s,"unsupported");goto done;}
        if(s->phase!=BR_FENCE_ACTIVE||s->ctl->phase!=BR_CTL_ACTIVE||h->generation<s->generation){rc=0;goto done;}
        if(h->generation>s->generation){close_gate(s,"unsupported");goto done;}
        /* Ring is local ringtone authority, never phone PCM permission. */
        if(!(s->confirmed_audio&2u)||h->sequence<=s->audio_sequence){rc=0;goto done;}
        s->audio_sequence=h->sequence;s->reply_ended=0;return 1;
    }
    if(h->type==BR_WIRE_CANCEL_ACK){
        int restored=s->phase==BR_FENCE_ACTIVE&&s->acked&&
            s->generation>s->cancel_generation&&s->ctl->phase==BR_CTL_ACTIVE&&s->ctl->cycle==s->cycle;
        if((s->phase!=BR_FENCE_FROZEN&&s->phase!=BR_FENCE_RESUMING&&!restored)||
           !s->cancel_generation||h->generation!=s->cancel_generation||
           h->sequence!=s->cancel_sequence){close_gate(s,"unsupported");goto done;}
        if(s->acked){rc=0;goto done;}
        s->acked=1;rc=3;goto done;
    }
    if(h->type==BR_WIRE_RESUMED){
        /* A second copy cannot acquire producer proof or restore S0 again.
         * Only the still-current, successfully restored cycle qualifies. */
        if(s->phase==BR_FENCE_ACTIVE&&s->ctl->phase==BR_CTL_ACTIVE&&s->ctl->cycle==s->cycle&&
           s->acked&&s->cancel_generation&&s->generation>s->cancel_generation&&
           h->generation==s->generation&&h->sequence==s->cancel_sequence){rc=0;goto done;}
        if(s->phase!=BR_FENCE_RESUMING||s->ctl->phase!=BR_CTL_RESUMING||
           h->generation!=s->next_generation||h->sequence!=s->cancel_sequence){close_gate(s,"unsupported");goto done;}
        s->busy=1;
        int proof=s->released(s->context,s->run,s->epoch,s->cycle,s->cancel_sequence,s->generation,s->next_generation);
        s->busy=0;
        if(proof!=1){close_gate(s,"bridge_unreachable");goto done;}
        s->busy=1;
        int restored=br_ctl_resumed(s->ctl,s->run,s->cycle,s->epoch,now);
        s->busy=0;
        if(restored!=BR_CTL_ACTIVE){close_gate(s,"bridge_unreachable");goto done;}
        s->generation=s->next_generation;s->next_generation=0;s->confirmed_audio=s->next_audio;
        s->next_audio=0;s->audio_sequence=s->mic_sequence=0;s->reply_ended=0;s->phase=BR_FENCE_ACTIVE;rc=4;goto done;
    }
    if(h->type==BR_WIRE_REPLY_END){
        if(s->phase!=BR_FENCE_ACTIVE||s->ctl->phase!=BR_CTL_ACTIVE||h->generation<s->generation){rc=0;goto done;}
        if(h->generation!=s->generation||!s->audio_sequence||h->sequence!=s->audio_sequence){close_gate(s,"unsupported");goto done;}
        if(s->reply_ended){rc=0;goto done;}
        s->reply_ended=1;rc=2;goto done;
    }
    close_gate(s,"unsupported");
done:
    memset(p,0,sizeof(*p));return rc;
}
int br_ctl_fence_mic(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t sequence,uint64_t now,
                     const void *pcm,size_t n,dc_wire_packet *p,void *wire,size_t capacity,size_t *length){
    int rc=-1;if(!p)return -1;memset(p,0,sizeof(*p));
    if(!identity(s,run,cycle))return -1;
    if(current(s,now)!=1||s->phase!=BR_FENCE_ACTIVE||s->ctl->phase!=BR_CTL_ACTIVE||
       !(s->confirmed_audio&1u)||!sequence||sequence<=s->mic_sequence||!pcm||!n||n>8192||(n&1u))return -1;
    p->header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=LX06_BRIDGE_WIRE_VERSION,
        .type=BR_WIRE_MIC,.flags=DC_WIRE_PCM_S16LE,.epoch=s->epoch,.generation=s->generation,
        .sequence=sequence,.length=(uint32_t)n,.rate=16000,.channels=1};
    memcpy(p->payload,pcm,n);
    if(br_wire_encode(p,BR_FROM_SPEAKER,s->epoch,NULL,NULL,wire,capacity,length)==BR_WIRE_OK){s->mic_sequence=sequence;rc=1;}
    memset(p,0,sizeof(*p));return rc;
}
int br_ctl_fence_poll(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t now){
    if(!identity(s,run,cycle))return -1;
    return current(s,now);
}
int br_ctl_fence_close(br_ctl_fence *s,uint64_t run,uint64_t cycle,uint64_t now){
    if(!identity(s,run,cycle))return -1;
    if(now>=s->now_ms&&now>=s->ctl->now_ms)s->now_ms=now;
    return close_gate(s,"bridge_unreachable");
}
