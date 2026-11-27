#include "discovery.h"
#include <string.h>
_Static_assert(sizeof(br_discovery)<170*1024,"bounded discovery memory");
static unsigned field(const br_json *d,unsigned n,const char *key){return br_json_field(d,n,key);}
static int text(const br_json *d,unsigned n,int nonempty){
    return br_json_kind(d,n)==BR_J_STRING&&(!nonempty||br_json_text(d,n).length);
}
static int private_keys(const br_json *d){
    for(unsigned i=1;i<=d->count;i++)
        if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return 1;
    return 0;
}
static const char *error_name(const br_json *d,unsigned n){
    static const char *const errors[]={"bridge_unreachable","token_invalid","bridge_connect_timeout",
        "app_not_installed","hook_unavailable","hook_version_mismatch","app_version_unsupported",
        "bridge_version_mismatch","session_missing","search_empty","op_timeout","script_disabled",
        "unsupported","quota_exhausted"};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++)
        if(br_json_is(d,n,errors[i]))return errors[i];
    return NULL;
}
void br_discovery_close(br_discovery *s){if(s)memset(s,0,sizeof(*s));}
static int stop(br_discovery *s,const char *error){
    br_discovery_close(s);s->error=error;return BR_DISC_CLOSED;
}
static int describe_fail(br_discovery *s,const char *error){
    s->describing=0;s->describe_failed=1;s->has_description=0;s->error=error;
    s->selector_app=0;
    memset(&s->description,0,sizeof(s->description));return BR_DISC_DESCRIBE_FAILED;
}
int br_discovery_begin(br_discovery *s,uint64_t epoch,uint64_t now){
    if(!s)return BR_DISC_INVALID;
    br_discovery_close(s);
    if(!epoch){s->error="unsupported";return BR_DISC_INVALID;}
    s->epoch=epoch;s->now_ms=now;s->started_ms=now;s->phase=BR_DISC_DIALING;
    br_caps_begin(&s->caps,epoch);return 1;
}
int br_discovery_poll(br_discovery *s,uint64_t now){
    if(!s)return BR_DISC_INVALID;
    if(s->phase==BR_DISC_OFF)return BR_DISC_CLOSED;
    if(now<s->now_ms)return stop(s,"bridge_unreachable");
    s->now_ms=now;
    if((s->phase==BR_DISC_DIALING&&now-s->started_ms>=10000)||
       (s->phase==BR_DISC_WAIT_CAPS&&now-s->started_ms>=5000)||
       (s->phase==BR_DISC_READY&&now-s->heartbeat_ms>=6000))return stop(s,"bridge_unreachable");
    if(s->describing&&now-s->describe_ms>=5000)return describe_fail(s,"op_timeout");
    return BR_DISC_NONE;
}
static int current(br_discovery *s,uint64_t epoch,uint64_t now){
    if(!s)return BR_DISC_INVALID;
    if(s->phase==BR_DISC_OFF)return BR_DISC_CLOSED;
    /* A stale asynchronous transport callback must not advance or regress the
     * new connection's clock, expire it, or erase its authority. */
    if(!epoch||epoch!=s->epoch)return BR_DISC_INVALID;
    int rc=br_discovery_poll(s,now);
    if(rc)return rc;
    return BR_DISC_NONE;
}
int br_discovery_authenticated(br_discovery *s,uint64_t epoch,uint64_t now){
    int rc=current(s,epoch,now);if(rc)return rc;
    if(s->phase!=BR_DISC_DIALING)return BR_DISC_INVALID;
    s->phase=BR_DISC_WAIT_CAPS;s->started_ms=now;s->heartbeat_ms=now;s->error=NULL;return 1;
}
int br_discovery_heartbeat(br_discovery *s,uint64_t epoch,uint64_t now){
    int rc=current(s,epoch,now);if(rc)return rc;
    if(s->phase!=BR_DISC_READY&&s->phase!=BR_DISC_WAIT_CAPS)return BR_DISC_INVALID;
    s->heartbeat_ms=now;return BR_DISC_HEARTBEAT;
}
int br_discovery_probe(br_discovery *s,uint64_t epoch,uint64_t now,void *out,size_t capacity,size_t *length){
    if(!s||!out||!length||capacity<64)return BR_DISC_INVALID;
    int rc=current(s,epoch,now);if(rc)return rc;
    if((s->phase!=BR_DISC_WAIT_CAPS&&s->phase!=BR_DISC_READY)||s->probe>=UINT64_MAX-1)return BR_DISC_INVALID;
    dc_wire_packet *p=&s->packet;memset(p,0,sizeof(*p));
    uint64_t next=(s->probe>>1)+1;
    p->header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=LX06_BRIDGE_WIRE_VERSION,
        .type=BR_WIRE_PING,.epoch=epoch,.length=16};
    for(unsigned i=0;i<8;i++){p->payload[i]=(unsigned char)(epoch>>(8*i));p->payload[8+i]=(unsigned char)(next>>(8*i));}
    rc=br_wire_encode(p,BR_FROM_SPEAKER,epoch,br_wire_heartbeat_check,NULL,out,capacity,length);
    memset(p,0,sizeof(*p));
    if(rc!=BR_WIRE_OK)return BR_DISC_INVALID;
    s->probe=(next<<1)|1;return 1;
}
int br_discovery_wire_heartbeat(br_discovery *s,uint64_t now){
    if(!s)return BR_DISC_INVALID;
    dc_wire_packet *p=&s->packet;
    if(p->header.epoch!=s->epoch||!s->epoch||now!=s->now_ms||
       !br_wire_heartbeat_check(NULL,&p->header,p->payload,p->header.length))return BR_DISC_INVALID;
    if(p->header.type==BR_WIRE_PING)return BR_DISC_HEARTBEAT;
    if(!(s->probe&1)||p->header.length!=16)return BR_DISC_NONE;
    uint64_t serial=s->probe>>1;
    for(unsigned i=0;i<8;i++)if(p->payload[i]!=(unsigned char)(s->epoch>>(8*i))||
        p->payload[8+i]!=(unsigned char)(serial>>(8*i)))return BR_DISC_NONE;
    s->probe&=~UINT64_C(1);s->heartbeat_ms=now;return BR_DISC_HEARTBEAT;
}
int br_discovery_describe_begin(br_discovery *s,uint64_t epoch,uint64_t now,const void *data,size_t length){
    int rc=current(s,epoch,now);if(rc)return rc;
    if(s->phase!=BR_DISC_READY||s->describing||s->describe_failed||s->business_used)return BR_DISC_INVALID;
    rc=br_caps_describe_request(&s->caps,epoch,data,length,&s->scratch);
    if(rc!=BR_CAPS_OK){s->error=br_caps_error(rc);return BR_DISC_INVALID;}
    /* Index into this discovery's immutable caps, never request/scratch. */
    if(br_json_parse(&s->scratch,data,length)!=1)return BR_DISC_INVALID;
    unsigned app=field(&s->scratch,1,"app"),selected=0;
    const br_json *c=&s->caps.document;
    unsigned apps=field(c,1,"apps");
    if(app)for(unsigned a=br_json_child(c,apps);a&&a<br_json_next(c,apps);a=br_json_next(c,a)){
        unsigned name=field(c,a,"app");
        if(br_json_equal(c,name,&s->scratch,app)){selected=name;break;}
    }
    if(app&&!selected){memset(&s->scratch,0,sizeof(s->scratch));return BR_DISC_INVALID;}
    s->selector_app=selected;
    memset(&s->scratch,0,sizeof(s->scratch));memset(&s->description,0,sizeof(s->description));
    s->has_description=0;s->describing=1;s->describe_ms=now;s->error=NULL;return 1;
}
/* Wire callback validates this profile's control subset, WITHOUT changing any
 * connection/caps state. Codec refusal must never leave half-installed caps.
 */
static int semantic(void *context,const dc_wire_header *h,const unsigned char *data,size_t length){
    br_discovery *s=context;br_json *d=&s->scratch;
    if(br_wire_heartbeat_type(h->type))return br_wire_heartbeat_check(NULL,h,data,length);
    if(br_json_parse(d,data,length)!=1||br_json_kind(d,1)!=BR_J_OBJECT||private_keys(d))return 0;
    switch(h->type){
    case BR_WIRE_GRANT:
        return br_json_is(d,field(d,1,"state"),"started")&&
               text(d,field(d,1,"command_id"),1)&&text(d,field(d,1,"task_id"),1);
    case BR_WIRE_READY:
        return (s->phase==BR_DISC_WAIT_CAPS&&br_json_is(d,field(d,1,"kind"),"caps"))||
               (s->phase==BR_DISC_READY&&s->describing&&br_json_is(d,field(d,1,"kind"),"state"));
    case BR_WIRE_ERROR:{
        unsigned msg=field(d,1,"msg");
        return error_name(d,field(d,1,"code"))&&(!msg||text(d,msg,0));
    }
    case BR_WIRE_STATE:{
        unsigned step=field(d,1,"step");uint64_t n;
        return !step||(br_json_kind(d,step)==BR_J_OBJECT&&
            br_json_u64(d,field(d,step,"n"),&n)&&n>0&&text(d,field(d,step,"text"),0));
    }
    case BR_WIRE_TEXT:return 1; /* diagnostics ONLY, never a result/ACK */
    default:return 0; /* business/audio/TASK belongs to the future full profile */
    }
}
static int selection(br_discovery *s){
    const br_json *d=&s->description,*c=&s->caps.document;
    unsigned apps=field(d,field(d,1,"json"),"apps"),count=0;
    for(unsigned a=br_json_child(d,apps);a&&a<br_json_next(d,apps);a=br_json_next(d,a)){
        count++;unsigned name=field(d,a,"app");
        if(s->selector_app&&!br_json_equal(c,s->selector_app,d,name))return 0;
        unsigned list=field(c,1,"apps"),found=0;
        for(unsigned b=br_json_child(c,list);b&&b<br_json_next(c,list);b=br_json_next(c,b)){
            if(!br_json_equal(d,name,c,field(c,b,"app")))continue;
            found=br_json_equal(d,field(d,a,"hook_version"),c,field(c,b,"hook_version"));break;
        }
        if(!found)return 0;
    }
    return !s->selector_app||count==1;
}
int br_discovery_receive(br_discovery *s,uint64_t epoch,uint64_t now,const void *data,size_t length){
    int rc=current(s,epoch,now);if(rc)return rc;
    if(s->phase!=BR_DISC_WAIT_CAPS&&s->phase!=BR_DISC_READY)return BR_DISC_INVALID;
    rc=br_wire_decode(data,length,BR_FROM_PHONE,s->epoch,semantic,s,&s->packet);
    if(rc!=BR_WIRE_OK)return stop(s,rc==BR_WIRE_VERSION?"bridge_version_mismatch":"unsupported");
    unsigned type=s->packet.header.type;
    switch(type){
    case BR_WIRE_READY:
        if(s->phase==BR_DISC_WAIT_CAPS){
            rc=br_caps_accept(&s->caps,s->epoch,type,s->packet.payload,s->packet.header.length);
            if(rc!=BR_CAPS_OK)return stop(s,br_caps_error(rc));
            s->phase=BR_DISC_READY;s->error=NULL;rc=BR_DISC_CAPS;
            /* Initial liveness starts at authentication, not arbitrary READY. */
        }else{
            if(br_describe_validate(&s->description,type,s->packet.payload,s->packet.header.length)!=1||!selection(s))
                rc=describe_fail(s,"unsupported");
            else{
                s->describing=0;s->has_description=1;s->error=NULL;rc=BR_DISC_DESCRIPTION;
                s->selector_app=0;
            }
        }
        break;
    case BR_WIRE_PING:case BR_WIRE_PONG:rc=br_discovery_wire_heartbeat(s,now);break;
    case BR_WIRE_GRANT:rc=BR_DISC_ACK;break;
    case BR_WIRE_STATE:rc=BR_DISC_PROGRESS;break;
    case BR_WIRE_TEXT:rc=BR_DISC_DIAGNOSTIC;break;
    case BR_WIRE_ERROR:{
        const char *error=error_name(&s->scratch,field(&s->scratch,1,"code"));
        if(s->phase==BR_DISC_WAIT_CAPS||!strcmp(error,"token_invalid")||!strcmp(error,"bridge_version_mismatch"))return stop(s,error);
        if(s->describing)rc=describe_fail(s,error);
        else{s->error=error;rc=BR_DISC_INVALID;}
        break;
    }
    default:rc=BR_DISC_INVALID;break;
    }
    memset(&s->scratch,0,sizeof(s->scratch));memset(&s->packet,0,sizeof(s->packet));return rc;
}
