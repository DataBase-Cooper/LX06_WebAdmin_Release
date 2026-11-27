#if !defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) && !defined(LX06_PHONE_PROFILE_RUNTIME)
#error "T10/T14: selected source has no production registration"
#endif
#include "profile_owner.h"
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
#include "profile_owner_flow.h"
#endif
#include "selected-script-profile.h"
#include <stdlib.h>
#include <string.h>
struct br_profile_owner {
    br_profile *profile;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    br_profile_flow *flow;
#endif
    br_discovery discovery;
    br_session session;
    br_operation one;int one_mode,one_owned,microphone_revoked;uint64_t microphone_sequence;
    br_session_io io;
    struct selected_script_profile *source;
    uint64_t run,epoch,last_run,last_epoch;
    char script[65],wake_source[17];
    unsigned char workspace[8240];
};
_Static_assert(sizeof(struct br_profile_owner)<=512u*1024u,"bounded original profile/session composition");
static int owner_identity(const br_profile_owner *o,uint64_t r,uint64_t e){
    return o&&r&&e&&o->run==r&&o->epoch==e;
}
static int owner_current(br_profile_owner *o){
    if(!o||!o->source)return 0;
    if(selected_script_current(o->source)==1)return 1;
    br_profile_close(o->profile,o->run,o->epoch);
    return 0;
}
static int text_equal(br_text a,const char *b){return a.data&&a.length==strlen(b)&&!memcmp(a.data,b,a.length);}
static int envelope_source(const void *raw,size_t n,const char *script,const char *source){
    br_json *j=calloc(1,sizeof(*j));if(!j)return 0;
    int ok=br_json_parse(j,raw,n)==1&&
        text_equal(br_json_text(j,br_json_field(j,1,"script_id")),script)&&
        text_equal(br_json_text(j,br_json_field(j,1,"wake_source")),source);
    memset(j,0,sizeof(*j));free(j);return ok;
}
br_profile_owner *br_profile_owner_new(br_session_io io){
    if(!io.receipt||!io.reaped||!io.close)return NULL;
    br_profile_owner *o=calloc(1,sizeof(*o));if(!o)return NULL;
    o->profile=br_profile_new();if(!o->profile){free(o);return NULL;}o->io=io;return o;
}
void br_profile_owner_close(br_profile_owner *o,uint64_t r,uint64_t e){
    if(!owner_identity(o,r,e))return;
    o->microphone_revoked=1;
    selected_script_tokens_retire(o->source,o,r,e);
    br_profile_close(o->profile,r,e);
    if(o->one_mode&&o->one_owned)o->io.close(o->io.context,r,e);
    /* A source may disappear between session_begin and profile attachment.
     * Close that original unattached session too; never reset its identity. */
    if(o->session.live)(void)br_session_close(&o->session,r,e);
    /* UNKNOWN retains source/CA/lock together with its ORIGINAL ticket. */
    if(!o->session.owned&&!o->one_owned
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
       &&!(o->flow&&o->flow->run.held)
#endif
    ){
        selected_script_release(o->source);o->source=NULL;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
        if(o->flow){o->flow->owner=NULL;o->flow=NULL;}
#endif
    }
}
int br_profile_owner_transport_closed(const br_profile_owner *o,uint64_t r,uint64_t e){
    return owner_identity(o,r,e)?br_profile_transport_closed(o->profile,r,e):BR_PROFILE_INVALID;
}
struct selected_source_token *br_profile_owner_source_token(br_profile_owner *o,uint64_t r,uint64_t e,const void *consumer){
    if(!owner_identity(o,r,e)||!o->source||br_profile_owner_transport_closed(o,r,e)!=0)return NULL;
    return selected_script_token_open(o->source,o,r,e,consumer);
}
int br_profile_owner_free(br_profile_owner *o){
    if(!o)return 0;
    br_profile_owner_close(o,o->run,o->epoch);
    if(o->session.owned||o->one_owned)return -1;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    if(o->flow&&o->flow->run.held)return -1;
#endif
    selected_script_release(o->source);br_profile_free(o->profile);memset(o,0,sizeof(*o));free(o);return 0;
}
static int owner_start(br_profile_owner *o,struct eo_saved_source *source,const dc_mode_request *q,
        uint64_t run,uint64_t epoch,const void *open,size_t n,int one_mode){
    if(!o||!run||run<=o->last_run||!epoch||epoch<=o->last_epoch)return BR_PROFILE_INVALID;
    /* Refusal consumes incoming identities without rebinding the incumbent or
     * its pending resource tickets. Only an admitted intent may close it. */
    o->last_run=run;o->last_epoch=epoch;
    if(!q||!memchr(q->script,0,sizeof q->script)||!memchr(q->source,0,sizeof q->source)||
       !memchr(q->wake,0,sizeof q->wake)||!memchr(q->target,0,sizeof q->target)||
       !memchr(q->action,0,sizeof q->action)||strcmp(q->action,"call.start"))return BR_PROFILE_INVALID;
    const char *ws=!strcmp(q->source,"local_wake")?"local":!strcmp(q->source,"official_asr")?"official":
        !strcmp(q->source,"manual")?"manual":NULL;
    if(!ws||!envelope_source(open,n,q->script,ws))return BR_PROFILE_INVALID;
    br_json *scratch=calloc(1,sizeof(*scratch));if(!scratch)return BR_PROFILE_INVALID;
    int valid=br_caps_discovery_request(epoch,open,n,scratch)==BR_CAPS_OK;
    free(scratch);if(!valid)return BR_PROFILE_INVALID;
    if(o->session.owned||o->one_owned)return BR_PROFILE_INVALID;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    if(o->flow)return BR_PROFILE_INVALID;
#endif
    struct selected_script_profile *next=selected_script_capture(source,q->script,q->source,q->wake,q->target,run,epoch);
    if(!next)return BR_PROFILE_INVALID;
    /* Admission precedes replacement. A same-source successor borrows the
     * retained settings lock; no second conflicting saved-source capture. */
    br_profile_owner_close(o,o->run,o->epoch);
    o->run=run;o->epoch=epoch;o->source=next;
    o->one_mode=one_mode;br_operation_clear(&o->one);
    struct selected_script_connection c;
    if(selected_script_connection(o->source,&c)){br_profile_owner_close(o,run,epoch);return BR_PROFILE_INVALID;}
    strcpy(o->script,q->script);strcpy(o->wake_source,ws);
    br_profile_config config={c.host,c.port,c.ca_file,c.token,{0}};
    memcpy(config.spki_sha256,c.spki,32);
    int rc=br_profile_dial_source(o->profile,&config,&o->discovery,run,epoch,open,n,
        selected_script_current,o->source);
    memset(&config,0,sizeof config);memset(&c,0,sizeof c);
    if(rc!=BR_PROFILE_CONNECTED)br_profile_owner_close(o,run,epoch);
    return rc;
}
int br_profile_owner_start(br_profile_owner *o,struct eo_saved_source *s,const dc_mode_request *q,uint64_t r,uint64_t e,const void *open,size_t n){
    return owner_start(o,s,q,r,e,open,n,0);
}
int br_profile_owner_start_one(br_profile_owner *o,struct eo_saved_source *s,const dc_mode_request *q,uint64_t r,uint64_t e,const void *open,size_t n){
    if(!o||o->run||o->source||o->session.owned||o->one_owned)return BR_PROFILE_INVALID;
    return owner_start(o,s,q,r,e,open,n,1);
}
const br_operation *br_profile_owner_operation(const br_profile_owner *o){return o&&o->one_mode?&o->one:NULL;}
int br_profile_owner_send_one(br_profile_owner *o,uint64_t r,uint64_t e,const void *open,size_t n){
    if(!owner_identity(o,r,e)||!o->one_mode||o->one_owned||!owner_current(o)||!o->one.discovery||
       !envelope_source(open,n,o->script,o->wake_source))return BR_PROFILE_INVALID;
    size_t length=0;int rc=br_operation_prepare(&o->one,e,o->discovery.now_ms,open,n,o->workspace,sizeof o->workspace,&length);
    if(rc!=BR_OP_PREPARED)return rc;
    o->one_owned=1; /* ownership before the one and only transport attempt */
    rc=br_profile_submit(o->profile,r,e,o->workspace,length);memset(o->workspace,0,sizeof o->workspace);
    if(rc!=1)br_profile_owner_close(o,r,e);return rc;
}
int br_profile_owner_microphone_allowed(br_profile_owner *o,uint64_t r,uint64_t e){
    if(!owner_identity(o,r,e))return BR_PROFILE_INVALID;
    if(o->microphone_revoked||!o->one_mode||!o->one_owned||!o->one.acked||
       o->one.error||o->one.plan.session_end||!(o->one.plan.requested_audio&BR_CAPS_MIC))return 0;
    if(o->one.completed){
        if(o->one.pending||o->one.confirmed_audio!=o->one.plan.requested_audio)return 0;
    }else if(!o->one.pending)return 0;
    if(!owner_current(o)||br_profile_owner_transport_closed(o,r,e)!=0)return BR_PROFILE_CLOSED;
    return 1;
}
void br_profile_owner_microphone_revoke(br_profile_owner *o,uint64_t r,uint64_t e){
    if(owner_identity(o,r,e))o->microphone_revoked=1;
}
int br_profile_owner_microphone(br_profile_owner *o,uint64_t r,uint64_t e,uint64_t sequence,
        const void *pcm,size_t bytes){
    int ready=br_profile_owner_microphone_allowed(o,r,e);
    if(ready!=1)return ready<0?ready:BR_PROFILE_INVALID;
    if(!pcm||!sequence||(bytes!=320&&bytes!=640&&bytes!=960))return BR_PROFILE_INVALID;
    if(sequence<=o->microphone_sequence)return 0;
    dc_wire_packet packet={0};packet.header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=1,
        .type=BR_WIRE_MIC,.epoch=e,.sequence=sequence,.length=(uint32_t)bytes};
    packet.header.flags=DC_WIRE_PCM_S16LE;packet.header.rate=16000;packet.header.channels=1;
    memcpy(packet.payload,pcm,bytes);size_t length=0;
    int encoded=br_wire_encode(&packet,BR_FROM_SPEAKER,e,NULL,NULL,o->workspace,sizeof o->workspace,&length);
    memset(&packet,0,sizeof packet);if(encoded!=BR_WIRE_OK)return BR_PROFILE_INVALID;
    /* Consume before the one actual transport attempt; no resend on failure. */
    o->microphone_sequence=sequence;
    int rc=br_profile_submit(o->profile,r,e,o->workspace,length);
    memset(o->workspace,0,sizeof o->workspace);
    if(rc!=1)br_profile_owner_close(o,r,e);return rc;
}
int br_profile_owner_pump_media(br_profile_owner *o,uint64_t r,uint64_t e,br_profile_fence_rx fence,void *context){
    if(!owner_identity(o,r,e))return BR_PROFILE_INVALID;
    if(!owner_current(o))return BR_PROFILE_CLOSED;
    int rc=br_profile_pump(o->profile,r,e,fence,context);
    if(rc==BR_DISC_CAPS&&o->one_mode&&!o->one.discovery){
        br_operation_init(&o->one,&o->discovery);
        if(br_profile_operation(o->profile,r,e,&o->one)!=1){br_profile_owner_close(o,r,e);return BR_PROFILE_CLOSED;}
    }
    if(rc==BR_DISC_CAPS&&!o->one_mode&&!o->session.live){
        if(br_session_begin(&o->session,&o->discovery,r,o->discovery.now_ms,o->io)!=1||
           br_profile_session(o->profile,r,e,&o->session)!=1){br_profile_owner_close(o,r,e);return BR_PROFILE_CLOSED;}
    }
    return rc;
}
int br_profile_owner_pump(br_profile_owner *o,uint64_t r,uint64_t e){
    return br_profile_owner_pump_media(o,r,e,NULL,NULL);
}
int br_profile_owner_send(br_profile_owner *o,uint64_t r,uint64_t e,uint64_t generation,const void *open,size_t n){
    if(!owner_identity(o,r,e))return BR_PROFILE_INVALID;
    if(!owner_current(o))return BR_PROFILE_CLOSED;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    if(o->flow)return BR_PROFILE_INVALID;
#endif
    if(!envelope_source(open,n,o->script,o->wake_source))return BR_PROFILE_INVALID;
    return br_session_send(&o->session,r,e,generation,o->discovery.now_ms,open,n,
        o->workspace,sizeof o->workspace,br_profile_submit,o->profile);
}
int br_profile_owner_release(br_profile_owner *o,uint64_t r,uint64_t e){
    if(!owner_identity(o,r,e))return BR_PROFILE_INVALID;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    if(o->flow)return BR_PROFILE_INVALID;
#endif
    if(o->one_mode){
        if(!o->one_owned||br_profile_owner_transport_closed(o,r,e)!=1)return BR_PROFILE_INVALID;
        if(o->io.reaped(o->io.context,r,e,o->one.command_id,o->one.task_id)!=1)return 0;
        o->one_owned=0;selected_script_release(o->source);o->source=NULL;return 1;
    }
    if(o->session.live&&owner_current(o)){
        int rc=br_session_release(&o->session,r,e,o->discovery.now_ms);
        /* The original reap callback can invalidate its source. Preserve the
         * real release fact, but do not return next-OPEN permission for it. */
        if(!owner_current(o)){br_profile_owner_close(o,r,e);return BR_PROFILE_CLOSED;}
        return rc;
    }
    if(!o->session.owned||!o->session.count)return BR_PROFILE_INVALID;
    unsigned index=o->session.count-1;
    int rc=br_session_release_closed(&o->session,r,e,index,o->session.tickets[index].generation);
    if(rc==1){selected_script_release(o->source);o->source=NULL;}
    return rc;
}
int br_profile_owner_controls(br_profile_owner *o,uint64_t r,uint64_t e,struct cfg_bridge_app *out){
    return owner_identity(o,r,e)&&owner_current(o)?selected_script_controls(o->source,out):-1;
}
const br_session *br_profile_owner_session(const br_profile_owner *o){return o?&o->session:NULL;}
const br_discovery *br_profile_owner_discovery(const br_profile_owner *o){return o?&o->discovery:NULL;}

#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
#include "profile_owner_flow.inc"
#endif
