#include "operation.h"
#include <string.h>
_Static_assert(sizeof(br_operation)+sizeof(br_discovery)<220*1024,"bounded control consumer");
static unsigned field(const br_json *d,unsigned n,const char *key){return br_json_field(d,n,key);}
static int object(const br_json *d,unsigned n){return br_json_kind(d,n)==BR_J_OBJECT;}
static int text(const br_json *d,unsigned n,int nonempty){
    return br_json_kind(d,n)==BR_J_STRING&&(!nonempty||br_json_text(d,n).length);
}
static int identity(const br_json *d,unsigned n){
    br_text t=br_json_text(d,n);
    if(!text(d,n,1)||t.length>128)return 0;
    for(size_t i=0;i<t.length;i++)if(t.data[i]<32||t.data[i]==127)return 0;
    return 1;
}
static int equal(const br_json *d,unsigned n,const char *s){
    br_text t=br_json_text(d,n);size_t length=strlen(s);
    return text(d,n,0)&&t.length==length&&!memcmp(t.data,s,length);
}
int br_operation_grant_check(const br_json *d,const char *command,const char *task){
    return d&&command&&object(d,1)&&br_json_is(d,field(d,1,"state"),"started")&&
        equal(d,field(d,1,"command_id"),command)&&identity(d,field(d,1,"task_id"))&&
        (!task||equal(d,field(d,1,"task_id"),task));
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
static void scratch_clear(br_discovery *d){
    memset(&d->scratch,0,sizeof(d->scratch));memset(&d->packet,0,sizeof(d->packet));
}
void br_operation_clear(br_operation *s){if(s)memset(s,0,sizeof(*s));}
void br_operation_init(br_operation *s,br_discovery *d){
    if(!s)return;
    br_operation_clear(s);s->discovery=d;s->epoch=d?d->epoch:0;
}
static int stop(br_operation *s,const char *error,int event){
    br_discovery *d=s->discovery;
    if(d&&(d->epoch==s->epoch||d->phase==BR_DISC_OFF)){br_discovery_close(d);d->error=error;}
    br_operation_clear(s);s->discovery=d;s->error=error;return event;
}
void br_operation_abort(br_operation *s){if(s)(void)stop(s,"bridge_unreachable",BR_OP_CLOSED);}
int br_operation_poll(br_operation *s,uint64_t now){
    if(!s||!s->discovery)return BR_OP_INVALID;
    br_discovery *d=s->discovery;
    /* Never let a detached old operation erase a replacement connection. */
    if(!s->epoch||s->epoch!=d->epoch)return BR_OP_INVALID;
    int rc=br_discovery_poll(d,now);
    if(rc==BR_DISC_CLOSED)return stop(s,d->error?d->error:"bridge_unreachable",BR_OP_CLOSED);
    if(rc||d->phase!=BR_DISC_READY||d->describing)return BR_OP_INVALID;
    if(s->pending&&now-s->started_ms>=s->timeout_ms)
        return stop(s,s->connecting?"bridge_connect_timeout":"op_timeout",BR_OP_TIMEOUT);
    return BR_OP_NONE;
}
static int current(br_operation *s,uint64_t epoch,uint64_t now){
    if(!s||!s->discovery||!epoch||epoch!=s->epoch||epoch!=s->discovery->epoch)return BR_OP_INVALID;
    return br_operation_poll(s,now);
}
static int encoded_open(void *context,const dc_wire_header *h,const unsigned char *data,size_t n){
    (void)context;(void)data;(void)n;return h->type==BR_WIRE_OPEN;
}
int br_operation_encode(const br_caps *caps,uint64_t epoch,uint64_t generation,int list,
    const void *data,size_t n,br_json *scratch,dc_wire_packet *p,br_caps_plan *out_plan,
    char out_id[37],void *wire,size_t capacity,size_t *written){
    br_caps_plan plan={0};char id[37]={0};int rc=BR_CAPS_INVALID;
    if(!scratch||!p)return rc;
    memset(scratch,0,sizeof(*scratch));memset(p,0,sizeof(*p));
    if(!data||!n||n>8192||!out_plan||!out_id||!wire||!written||capacity<48+n||(list!=0&&list!=1))goto done;
    rc=list?br_caps_task_list_request(caps,epoch,data,n,scratch):
        br_caps_open_operation(caps,epoch,data,n,scratch,&plan);
    if(rc!=BR_CAPS_OK)goto done;
    rc=BR_CAPS_INVALID;
    if(br_json_parse(scratch,data,n)!=1)goto done;
    br_text command=br_json_text(scratch,field(scratch,1,"command_id"));
    if(command.length!=36)goto done;
    memcpy(id,command.data,36);
    p->header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=LX06_BRIDGE_WIRE_VERSION,
        .type=BR_WIRE_OPEN,.epoch=epoch,.generation=generation,.length=(uint32_t)n};
    memcpy(p->payload,data,n);
    if(br_wire_encode(p,BR_FROM_SPEAKER,epoch,encoded_open,NULL,wire,capacity,written)!=BR_WIRE_OK)goto done;
    *out_plan=plan;memcpy(out_id,id,sizeof(id));rc=BR_CAPS_OK;
done:
    memset(scratch,0,sizeof(*scratch));memset(p,0,sizeof(*p));return rc;
}
static int prepare(br_operation *s,uint64_t epoch,uint64_t now,const void *data,size_t length,
                    void *wire,size_t capacity,size_t *written,int list){
    if(s){s->last_event=0;memset(&s->response,0,sizeof(s->response));}
    int rc=current(s,epoch,now);if(rc)return rc;
    br_discovery *d=s->discovery;
    if(d->business_used||s->pending||!data||!wire||!written||!length||length>8192||capacity<48+length)return BR_OP_INVALID;
    rc=br_operation_encode(&d->caps,epoch,0,list,data,length,&d->scratch,&d->packet,
                           &s->plan,s->command_id,wire,capacity,written);
    if(rc!=BR_CAPS_OK){s->error=br_caps_error(rc);return BR_OP_INVALID;}
    s->list_mode=list;
    scratch_clear(d);memset(&s->response,0,sizeof(s->response));
    s->connecting=!list&&s->plan.connection_op.length==s->plan.op.length&&s->plan.op.length&&
        !memcmp(s->plan.connection_op.data,s->plan.op.data,s->plan.op.length);
    s->timeout_ms=list?5000:s->connecting?s->plan.connection_timeout_ms:s->plan.timeout_ms;
    s->started_ms=now;s->pending=1;d->business_used=1;s->error=NULL;s->last_event=0;
    return BR_OP_PREPARED;
}
int br_operation_prepare(br_operation *s,uint64_t epoch,uint64_t now,const void *data,size_t length,
                         void *wire,size_t capacity,size_t *written){
    return prepare(s,epoch,now,data,length,wire,capacity,written,0);
}
int br_operation_prepare_list(br_operation *s,uint64_t epoch,uint64_t now,const void *data,size_t length,
                              void *wire,size_t capacity,size_t *written){
    return prepare(s,epoch,now,data,length,wire,capacity,written,1);
}
static int audio(const br_json *d,unsigned n,unsigned *out){
    *out=0;if(!n)return 1;
    if(!object(d,n))return 0;
    static const char *const keys[]={"mic","speaker","ring"};
    for(unsigned k=br_json_child(d,n);k&&k<br_json_next(d,n);k=br_json_next(d,k+1)){
        unsigned i;for(i=0;i<3;i++)if(br_json_is(d,k,keys[i]))break;
        if(i==3||(!br_json_is(d,k+1,"open")&&!br_json_is(d,k+1,"close")))return 0;
        if(br_json_is(d,k+1,"open"))*out|=1u<<i;
    }
    return 1;
}
static int correlation(const br_operation *s,const br_json *d){
    unsigned c=field(d,1,"command_id"),t=field(d,1,"task_id");
    return (!c||equal(d,c,s->command_id))&&(!t||(identity(d,t)&&(!s->acked||equal(d,t,s->task_id))));
}
static int capable(const br_operation *s,unsigned bits){
    const br_json *d=&s->discovery->caps.document;
    unsigned apps=field(d,1,"apps");
    for(unsigned a=br_json_child(d,apps);a&&a<br_json_next(d,apps);a=br_json_next(d,a)){
        br_text name=br_json_text(d,field(d,a,"app"));
        if(name.length!=s->plan.app.length||memcmp(name.data,s->plan.app.data,name.length))continue;
        static const char *const keys[]={"mic","speaker","ring"};
        for(unsigned i=0;i<3;i++)if((bits&(1u<<i))&&br_json_is(d,field(d,field(d,a,"audio"),keys[i]),"none"))return 0;
        return 1;
    }
    return 0;
}
static int semantic(void *context,const dc_wire_header *h,const unsigned char *data,size_t length){
    br_operation *s=context;br_json *d=&s->discovery->scratch;unsigned bits;
    if(br_wire_heartbeat_type(h->type))return br_wire_heartbeat_check(NULL,h,data,length);
    if(br_json_parse(d,data,length)!=1||!object(d,1))return 0;
    for(unsigned i=1;i<=d->count;i++)
        if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return 0;
    switch(h->type){
    case BR_WIRE_TEXT:return 1;
    case BR_WIRE_GRANT:
        return s->pending&&br_operation_grant_check(d,s->command_id,s->acked?s->task_id:NULL);
    case BR_WIRE_READY:
        if(s->list_mode){
            unsigned tasks=field(d,1,"tasks"),count=0;
            if(!s->pending||!br_json_is(d,field(d,1,"kind"),"tasks")||
               br_json_kind(d,tasks)!=BR_J_ARRAY||!correlation(s,d)||field(d,1,"audio")||field(d,1,"set"))return 0;
            for(unsigned i=br_json_child(d,tasks);i&&i<br_json_next(d,tasks);i=br_json_next(d,i))
                if(++count>64||!object(d,i))return 0;
            return 1; /* row admission/durable receipts belong to K6 */
        }
        return s->pending&&!s->plan.oneway&&
            (br_json_is(d,field(d,1,"kind"),"result")||br_json_is(d,field(d,1,"kind"),"state"))&&correlation(s,d)&&
            audio(d,field(d,1,"audio"),&bits)&&capable(s,bits)&&(!s->plan.silent||!bits)&&
            (!field(d,1,"set")||object(d,field(d,1,"set")))&&
            (!field(d,1,"json")||object(d,field(d,1,"json")))&&
            (!field(d,1,"text")||text(d,field(d,1,"text"),0));
    case BR_WIRE_ERROR:
        return s->pending&&error_name(d,field(d,1,"code"))&&correlation(s,d)&&
            (!field(d,1,"msg")||text(d,field(d,1,"msg"),0));
    case BR_WIRE_STATE:{
        unsigned step=field(d,1,"step");uint64_t n;
        return correlation(s,d)&&(!step||(object(d,step)&&br_json_u64(d,field(d,step,"n"),&n)&&
            n>0&&text(d,field(d,step,"text"),0)));
    }
    case BR_WIRE_TASK:{
        if(br_json_is(d,field(d,1,"kind"),"notify"))return 1;
        uint64_t epoch;
        return br_json_is(d,field(d,1,"kind"),"result")&&identity(d,field(d,1,"task_id"))&&
            identity(d,field(d,1,"command_id"))&&br_json_u64(d,field(d,1,"epoch"),&epoch)&&epoch&&
            identity(d,field(d,1,"app_version"))&&identity(d,field(d,1,"hook_version"))&&
            (!field(d,1,"text")||text(d,field(d,1,"text"),0))&&
            (!field(d,1,"json")||object(d,field(d,1,"json")))&&
            (!field(d,1,"code")||error_name(d,field(d,1,"code")));
    }
    default:return 0;
    }
}
int br_operation_receive(br_operation *s,uint64_t epoch,uint64_t now,const void *data,size_t length){
    /* A refused receive invalidates borrowed delivery data. Stale callbacks
     * still cannot change the replacement discovery's authority. */
    if(s){s->last_event=0;memset(&s->response,0,sizeof(s->response));}
    int rc=current(s,epoch,now);if(rc)return rc;
    br_discovery *d=s->discovery;
    memset(&s->response,0,sizeof(s->response));
    rc=br_wire_decode(data,length,BR_FROM_PHONE,epoch,semantic,s,&d->packet);
    if(rc!=BR_WIRE_OK)return stop(s,rc==BR_WIRE_VERSION?"bridge_version_mismatch":"unsupported",BR_OP_CLOSED);
    unsigned type=d->packet.header.type;
    switch(type){
    case BR_WIRE_PING:case BR_WIRE_PONG:
        rc=br_discovery_wire_heartbeat(d,now)==BR_DISC_HEARTBEAT?BR_OP_HEARTBEAT:BR_OP_NONE;break;
    case BR_WIRE_GRANT:{
        br_text t=br_json_text(&d->scratch,field(&d->scratch,1,"task_id"));
        memcpy(s->task_id,t.data,t.length);s->task_id[t.length]=0;s->acked=1;rc=BR_OP_ACK;break;
    }
    case BR_WIRE_READY:
        (void)audio(&d->scratch,field(&d->scratch,1,"audio"),&s->confirmed_audio);
        s->pending=0;s->completed=1;s->error=NULL;s->response=d->scratch;rc=s->list_mode?BR_OP_TASKS:BR_OP_READY;break;
    case BR_WIRE_ERROR:{
        const char *error=error_name(&d->scratch,field(&d->scratch,1,"code"));
        if(!strcmp(error,"token_invalid")||!strcmp(error,"bridge_version_mismatch"))return stop(s,error,BR_OP_CLOSED);
        s->pending=0;s->error=error;s->response=d->scratch;rc=BR_OP_ERROR;break;
    }
    case BR_WIRE_STATE:s->response=d->scratch;rc=BR_OP_PROGRESS;break;
    case BR_WIRE_TASK:s->response=d->scratch;rc=BR_OP_TASK;break;
    case BR_WIRE_TEXT:rc=BR_OP_DIAGNOSTIC;break;
    default:return stop(s,"unsupported",BR_OP_CLOSED);
    }
    s->last_event=rc;scratch_clear(d);return rc;
}
