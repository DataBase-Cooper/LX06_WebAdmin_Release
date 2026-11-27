#include "session.h"
#include "result_fingerprint.h"
#include <string.h>
_Static_assert(sizeof(br_session)<64*1024,"bounded session + original response");
static unsigned field(const br_json *d,const char *key){return br_json_field(d,1,key);}
static int equal(br_text t,const char *s){return t.length&&t.length==strlen(s)&&!memcmp(t.data,s,t.length);}
static int name(br_text t){
    if(!t.length||t.length>128)return 0;
    for(size_t i=0;i<t.length;i++)if(t.data[i]<32||t.data[i]==127)return 0;
    return 1;
}
static void scratch_clear(br_discovery *d){memset(&d->scratch,0,sizeof(d->scratch));memset(&d->packet,0,sizeof(d->packet));}
static int identity(const br_session *s,uint64_t run,uint64_t epoch){
    return s&&!s->busy&&s->live&&run&&run==s->run&&epoch&&epoch==s->epoch&&
        s->operation.discovery&&s->operation.discovery->epoch==epoch;
}
static int stop(br_session *s,const char *error,int event){
    s->live=0;s->error=error;
    br_discovery *d=s->operation.discovery;
    /* A replaced connection is never closed by an old callback. */
    if(d&&(!d->epoch||d->epoch==s->epoch))br_operation_abort(&s->operation);
    s->busy=1;s->io.close(s->io.context,s->run,s->epoch);s->busy=0;return event;
}
void br_session_init(br_session *s){if(s)memset(s,0,sizeof(*s));}
int br_session_begin(br_session *s,br_discovery *d,uint64_t run,uint64_t now,br_session_io io){
    if(!s||s->busy||s->live||s->owned||!d||!run||run<=s->last_run||!d->epoch||d->epoch==s->last_epoch||
       d->phase!=BR_DISC_READY||d->business_used||d->describing||!io.receipt||!io.reaped||!io.close)return BR_OP_INVALID;
    if(!br_caps_has_feature(&d->caps,d->epoch,"session.command_id")){s->error="unsupported";return BR_OP_INVALID;}
    if(br_discovery_poll(d,now)!=BR_DISC_NONE)return BR_OP_INVALID;
    br_session_init(s);s->io=io;s->run=s->last_run=run;s->epoch=s->last_epoch=d->epoch;
    br_operation_init(&s->operation,d);d->business_used=1;s->live=1;return 1;
}
int br_session_poll(br_session *s,uint64_t run,uint64_t epoch,uint64_t now){
    if(!identity(s,run,epoch))return BR_OP_INVALID;
    memset(&s->operation.response,0,sizeof(s->operation.response));s->operation.last_event=0;
    int rc=br_operation_poll(&s->operation,now);
    if(rc==BR_OP_CLOSED||rc==BR_OP_TIMEOUT)return stop(s,s->operation.error,rc);
    if(rc!=BR_OP_NONE)return stop(s,"unsupported",BR_OP_CLOSED);
    return BR_OP_NONE;
}
static int current(br_session *s,uint64_t run,uint64_t epoch,uint64_t now){
    return br_session_poll(s,run,epoch,now);
}
static int proof_current(const br_session *s,uint64_t run,uint64_t epoch){
    const br_discovery *d=s->operation.discovery;
    /* A resource/receipt proof is not authority for a connection lost while
     * the original owner looked it up. Never poll or erase a successor here.
     * Closed-ticket release deliberately does not need live authority. */
    return identity(s,run,epoch)&&d->phase==BR_DISC_READY&&!d->describing&&
        br_caps_has_feature(&d->caps,epoch,"session.command_id");
}
int br_session_prepare(br_session *s,uint64_t run,uint64_t epoch,uint64_t generation,uint64_t now,
    const void *data,size_t n,void *wire,size_t capacity,size_t *length){
    int rc=current(s,run,epoch,now);if(rc)return rc;
    br_operation *o=&s->operation;br_discovery *d=o->discovery;
    if(s->owned||s->ended||s->count==BR_SESSION_LIMIT||!generation||generation<s->generation||
       !data||!n||n>8192)return BR_OP_INVALID;
    /* Preflight history and script before the shared encoder writes output. */
    if(br_json_parse(&d->scratch,data,n)!=1){scratch_clear(d);return BR_OP_INVALID;}
    br_text id=br_json_text(&d->scratch,field(&d->scratch,"command_id"));
    br_text script=br_json_text(&d->scratch,field(&d->scratch,"script_id"));
    int valid=name(script)&&(!s->count||equal(script,s->script));
    for(unsigned i=0;i<s->count;i++)if(equal(id,s->tickets[i].command))valid=0;
    char saved_script[129]={0};if(valid)memcpy(saved_script,script.data,script.length);
    scratch_clear(d);if(!valid)return BR_OP_INVALID;
    br_session_ticket *t=&s->tickets[s->count];
    rc=br_operation_encode(&d->caps,epoch,generation,0,data,n,&d->scratch,&d->packet,
                            &o->plan,t->command,wire,capacity,length);
    if(rc!=BR_CAPS_OK){s->error=br_caps_error(rc);return BR_OP_INVALID;}
    t->generation=generation;s->generation=generation;s->count++;s->owned=1;s->terminal=0;
    memcpy(s->script,saved_script,sizeof(s->script));memcpy(o->command_id,t->command,sizeof(o->command_id));
    o->connecting=o->plan.op.length==o->plan.connection_op.length&&o->plan.op.length&&
        !memcmp(o->plan.op.data,o->plan.connection_op.data,o->plan.op.length);
    o->timeout_ms=o->connecting?o->plan.connection_timeout_ms:o->plan.timeout_ms;
    o->started_ms=now;o->pending=1;s->error=NULL;return BR_OP_PREPARED;
}
int br_session_send(br_session *s,uint64_t run,uint64_t epoch,uint64_t generation,uint64_t now,
    const void *open,size_t n,void *workspace,size_t capacity,br_session_send_io send,void *context){
    if(!send||!workspace||!capacity||capacity>8240)return BR_OP_INVALID;
    size_t length=0;
    int rc=br_session_prepare(s,run,epoch,generation,now,open,n,workspace,capacity,&length);
    if(rc!=BR_OP_PREPARED)return rc;
    unsigned index=s->count-1;
    /* Prepare committed the original ticket BEFORE transport can see bytes.
     * Block all session APIs during this nonblocking callback, not the caller's
     * event loop. No retry even if transport claims zero bytes were accepted. */
    s->busy=1;rc=send(context,run,epoch,workspace,length);s->busy=0;
    memset(workspace,0,length);
    if(rc!=1)return br_session_close_ticket(s,run,epoch,index,generation);
    return BR_OP_PREPARED;
}
static int control(unsigned type){return type==BR_WIRE_GRANT||type==BR_WIRE_READY||type==BR_WIRE_ERROR||type==BR_WIRE_STATE;}
static int preflight(void *ctx,const dc_wire_header *h,const unsigned char *data,size_t n){
    br_session *s=ctx;br_json *d=&s->operation.discovery->scratch;
    if(h->flags||h->sequence)return 0;
    if(br_wire_heartbeat_type(h->type))return br_wire_heartbeat_check(NULL,h,data,n);
    if(br_json_parse(d,data,n)!=1||br_json_kind(d,1)!=BR_J_OBJECT)return 0;
    for(unsigned i=1;i<=d->count;i++)if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return 0;
    if(!control(h->type))return h->type==BR_WIRE_TASK||h->type==BR_WIRE_TEXT;
    br_text id=br_json_text(d,field(d,"command_id"));
    for(unsigned i=0;i<s->count;i++)if(equal(id,s->tickets[i].command)){
        const br_session_ticket *t=&s->tickets[i];
        return h->generation==t->generation&&(h->type!=BR_WIRE_GRANT||
            br_operation_grant_check(d,t->command,t->task[0]?t->task:NULL));
    }
    return 0;
}
int br_session_receive(br_session *s,uint64_t run,uint64_t epoch,uint64_t now,const void *data,size_t n){
    int rc=current(s,run,epoch,now);if(rc)return rc;
    br_operation *o=&s->operation;br_discovery *d=o->discovery;
    rc=br_wire_decode(data,n,BR_FROM_PHONE,epoch,preflight,s,&d->packet);
    if(rc!=BR_WIRE_OK){scratch_clear(d);return stop(s,rc==BR_WIRE_VERSION?"bridge_version_mismatch":"unsupported",BR_OP_CLOSED);}
    unsigned type=d->packet.header.type;br_session_ticket *ticket=NULL;
    unsigned char grant_hash[32]={0};
    if(control(type)){
        br_text id=br_json_text(&d->scratch,field(&d->scratch,"command_id"));
        for(unsigned i=0;i<s->count;i++)if(equal(id,s->tickets[i].command)){ticket=&s->tickets[i];break;}
        /* History drops must not hide a conflicting task identity. A retired
         * unbound ticket cannot acquire a task from a late GRANT or READY. */
        int active=ticket&&ticket==&s->tickets[s->count-1]&&s->owned&&!s->terminal;
        unsigned node=field(&d->scratch,"task_id");br_text task=br_json_text(&d->scratch,node);
        int binds=type==BR_WIRE_GRANT||type==BR_WIRE_READY;
        int valid=ticket&&(ticket->task[0]?(name(task)&&equal(task,ticket->task)):
            (binds?(active&&name(task)):!node));
        for(unsigned i=0;i<s->count;i++)
            if(ticket!=&s->tickets[i]&&node&&equal(task,s->tickets[i].task))valid=0;
        if(!valid){scratch_clear(d);return stop(s,"unsupported",BR_OP_CLOSED);}
        if(type==BR_WIRE_GRANT){
            br_result_fingerprint(&d->scratch,grant_hash);
            if(ticket->grant_seen&&memcmp(grant_hash,ticket->grant_hash,sizeof grant_hash)){
                scratch_clear(d);return stop(s,"unsupported",BR_OP_CLOSED);
            }
        }
        if(!active){
            /* READY may precede its first GRANT. Identity is already bound;
             * retain only comparison evidence, without reporting ACK or
             * modifying operation/task/result/resource authority. */
            if(type==BR_WIRE_GRANT&&!ticket->grant_seen){
                memcpy(ticket->grant_hash,grant_hash,sizeof grant_hash);ticket->grant_seen=1;
            }
            if(type==BR_WIRE_READY||type==BR_WIRE_ERROR){
                unsigned char hash[32];br_result_fingerprint(&d->scratch,hash);
                int same=ticket->result_type==type&&!memcmp(hash,ticket->result_hash,sizeof hash);
                memset(hash,0,sizeof hash);
                if(!same){scratch_clear(d);return stop(s,"unsupported",BR_OP_CLOSED);}
            }
            scratch_clear(d);return BR_OP_NONE;
        }
        if(type==BR_WIRE_GRANT&&o->acked){scratch_clear(d);return BR_OP_NONE;}
        /* Do not bind until the original consumer also validates semantics. */
    }
    scratch_clear(d);
    rc=br_operation_receive(o,epoch,now,data,n);
    if(rc==BR_OP_CLOSED||rc==BR_OP_TIMEOUT)return stop(s,o->error,rc);
    if(rc==BR_OP_ACK&&ticket){
        memcpy(ticket->task,o->task_id,sizeof(ticket->task));
        memcpy(ticket->grant_hash,grant_hash,sizeof grant_hash);ticket->grant_seen=1;
    }
    if(rc==BR_OP_READY&&ticket){
        br_text task=br_json_text(&o->response,field(&o->response,"task_id"));
        memcpy(ticket->task,task.data,task.length);ticket->task[task.length]=0;
        memcpy(o->task_id,ticket->task,sizeof(o->task_id));s->terminal=1;
    }
    if(rc==BR_OP_ERROR){s->terminal=1;s->error=o->error;}
    if((rc==BR_OP_READY||rc==BR_OP_ERROR)&&ticket){
        br_result_fingerprint(&o->response,ticket->result_hash);ticket->result_type=type;
    }
    return rc;
}
int br_session_ledger(br_session *s,uint64_t run,uint64_t epoch,uint64_t now){
    int rc=current(s,run,epoch,now);if(rc)return rc;
    br_operation *o=&s->operation;
    if(!s->owned||s->terminal||!o->plan.oneway||!o->acked)return BR_OP_INVALID;
    s->busy=1;rc=s->io.receipt(s->io.context,run,epoch,o->command_id,o->task_id);s->busy=0;
    if(!proof_current(s,run,epoch))return stop(s,"bridge_unreachable",BR_OP_CLOSED);
    if(!rc)return 0;
    if(rc!=1)return stop(s,"unsupported",BR_OP_CLOSED);
    o->pending=0;o->completed=1;s->terminal=1;return 1;
}
int br_session_release(br_session *s,uint64_t run,uint64_t epoch,uint64_t now){
    int rc=current(s,run,epoch,now);if(rc)return rc;
    br_operation *o=&s->operation;
    if(!s->owned||!s->terminal)return BR_OP_INVALID;
    s->busy=1;rc=s->io.reaped(s->io.context,run,epoch,o->command_id,o->task_id);s->busy=0;
    if(!proof_current(s,run,epoch))return stop(s,"bridge_unreachable",BR_OP_CLOSED);
    if(!rc)return 0;
    if(rc!=1)return stop(s,"bridge_unreachable",BR_OP_CLOSED);
    s->ended=o->plan.session_end;s->owned=0;s->terminal=0;
    br_operation_init(o,o->discovery);return s->ended?2:1;
}
int br_session_close(br_session *s,uint64_t run,uint64_t epoch){
    if(!identity(s,run,epoch))return BR_OP_INVALID;
    return stop(s,"bridge_unreachable",BR_OP_CLOSED);
}
static int owned_ticket(const br_session *s,uint64_t run,uint64_t epoch,unsigned index,uint64_t generation){
    return s&&!s->busy&&s->owned&&run&&s->run==run&&epoch&&s->epoch==epoch&&
       s->count&&s->count<=BR_SESSION_LIMIT&&index==s->count-1&&generation&&
       s->generation==generation&&s->tickets[index].generation==generation;
}
int br_session_close_ticket(br_session *s,uint64_t run,uint64_t epoch,unsigned index,uint64_t generation){
    if(!owned_ticket(s,run,epoch,index,generation)||!s->live||!s->io.close)return -1;
    return stop(s,"bridge_unreachable",BR_OP_CLOSED);
}
int br_session_release_closed(br_session *s,uint64_t run,uint64_t epoch,unsigned index,uint64_t generation){
    if(!owned_ticket(s,run,epoch,index,generation)||s->live||!s->io.reaped)return -1;
    const br_session_ticket *t=s->tickets+index;
    s->busy=1;int rc=s->io.reaped(s->io.context,run,epoch,t->command,t->task);s->busy=0;
    if(rc!=1)return rc==0?0:-1;
    s->owned=0;s->terminal=0;br_operation_init(&s->operation,s->operation.discovery);return 1;
}
