#include "result_delivery.h"
#include <string.h>
_Static_assert(sizeof(br_delivery)<4096,"bounded result delivery scratch");
static unsigned field(const br_json *d,unsigned n,const char *key){return br_json_field(d,n,key);}
static int copy(const br_json *d,unsigned n,char *out,size_t capacity,int required){
    if(!n)return !required;
    if(br_json_kind(d,n)!=BR_J_STRING)return 0;
    br_text t=br_json_text(d,n);
    if(t.length>=capacity||memchr(t.data,0,t.length)||(required&&!t.length))return 0;
    memcpy(out,t.data,t.length);out[t.length]=0;return 1;
}
static int row(const br_json *d,unsigned n,br_result *r){
    memset(r,0,sizeof(*r));
    if(br_json_kind(d,n)!=BR_J_OBJECT||!br_json_is(d,field(d,n,"kind"),"result"))return 0;
    unsigned json=field(d,n,"json");
    if(json&&br_json_kind(d,json)!=BR_J_OBJECT)return 0;
    if(!copy(d,field(d,n,"task_id"),r->task_id,sizeof(r->task_id),1)||
       !copy(d,field(d,n,"command_id"),r->command_id,sizeof(r->command_id),1)||
       !copy(d,field(d,n,"app_version"),r->app_version,sizeof(r->app_version),1)||
       !copy(d,field(d,n,"hook_version"),r->hook_version,sizeof(r->hook_version),1)||
       !copy(d,field(d,n,"code"),r->code,sizeof(r->code),0)||
       !copy(d,field(d,n,"text"),r->text,sizeof(r->text),0)||
       !br_json_u64(d,field(d,n,"epoch"),&r->epoch))return 0;
    return br_result_valid(r);
}
int br_delivery_decode(const br_json *d,unsigned n,br_result *r){
    if(!r)return 0;
    if(d&&row(d,n,r))return 1;
    memset(r,0,sizeof(*r));return 0;
}
static int finish(br_delivery *s,int rc){
    if(rc==BR_DELIVERY_INVALID)s->report.last_rc=BR_INVALID;
    memset(&s->scratch,0,sizeof(s->scratch));memset(s->records,0,sizeof(s->records));return rc;
}
int br_delivery_accept(br_delivery *s,const br_operation *op,br_inbox *q,uint64_t now,
                       int trusted,br_delivery_policy policy,void *context){
    if(!s)return BR_DELIVERY_INVALID;
    memset(s,0,sizeof(*s));
    if(!op||!q||!policy||(trusted!=0&&trusted!=1)||now>UINT64_MAX-UINT64_C(86400000))return finish(s,BR_DELIVERY_INVALID);
    const br_discovery *c=op->discovery;
    if(!c||!op->epoch||c->epoch!=op->epoch||c->phase!=BR_DISC_READY||
       !c->caps.ready||c->caps.connection!=op->epoch)return finish(s,BR_DELIVERY_INVALID);
    const br_json *d=&op->response;
    if(op->last_event==BR_OP_TASK){
        if(br_json_is(d,field(d,1,"kind"),"notify"))return BR_DELIVERY_IGNORED;
        s->records[s->report.total++]=1;
    }else if(op->last_event==BR_OP_TASKS&&op->list_mode&&op->completed){
        unsigned tasks=field(d,1,"tasks");
        if(!br_json_is(d,field(d,1,"kind"),"tasks")||br_json_kind(d,tasks)!=BR_J_ARRAY)return finish(s,BR_DELIVERY_INVALID);
        for(unsigned n=br_json_child(d,tasks);n&&n<br_json_next(d,tasks);n=br_json_next(d,n)){
            if(s->report.total==BR_INBOX_ROWS)return finish(s,BR_DELIVERY_INVALID);
            s->records[s->report.total++]=(uint16_t)n;
        }
    }else return BR_DELIVERY_IGNORED;
    /* Do not persist an earlier row when a later row is malformed. */
    for(unsigned i=0;i<s->report.total;i++){
        if(!br_delivery_decode(d,s->records[i],&s->scratch)||(op->last_event==BR_OP_TASK&&s->scratch.epoch!=op->epoch)){
            s->report.stopped_at=i;return finish(s,BR_DELIVERY_INVALID);
        }
    }
    for(unsigned i=0;i<s->report.total;i++){
        s->report.stopped_at=i;
        (void)br_delivery_decode(d,s->records[i],&s->scratch); /* same validated doc */
        int audible=-1;unsigned ttl=0;
        if(policy(context,&s->scratch,&audible,&ttl)!=1||(audible!=0&&audible!=1)||ttl<60||ttl>86400)
            return finish(s,BR_DELIVERY_INVALID);
        int rc=br_inbox_receive(q,&s->scratch,op->last_event==BR_OP_TASK,op->epoch,now,trusted,audible,ttl);
        s->report.last_rc=rc;
        if(rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT){s->report.accepted++;s->report.durable++;}
        else if(rc==BR_DUPLICATE){s->report.duplicates++;s->report.durable++;}
        else return finish(s,rc==BR_IO?BR_DELIVERY_IO:rc==BR_INVALID?BR_DELIVERY_INVALID:BR_DELIVERY_DEFERRED);
    }
    s->report.stopped_at=s->report.total;return finish(s,BR_DELIVERY_COMPLETE);
}
