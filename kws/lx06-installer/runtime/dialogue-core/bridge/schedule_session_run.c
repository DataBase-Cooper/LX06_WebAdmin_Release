#include "schedule_session.h"
#include "schedule_owner.h"
#include "result_delivery.h"
#include "result_fingerprint.h"
#include <string.h>
static int prepare(void *p,const br_flow *f,unsigned event){
    br_schedule_session *x=p;
    if(!x||!x->attached||f!=x->adapter->flow||event!=BR_FLOW_PREPARE)return -1;
    return br_schedule_owner_prepare(x->owner,x->log,&x->binding,x->command)==1?1:-1;
}
static int identity(br_schedule_session *x,uint64_t run){
    if(!x||x->busy||!x->attached||!run||x->binding.run!=run)return 0;
    br_schedule_runner *r=x->runner;br_schedule *s=r->scripts[r->script];
    const br_schedule_record *rec=s->records+r->slot;
    return r->run==run&&r->flow_active&&r->serial==x->binding.serial&&
        r->epoch==x->adapter->epoch&&!strcmp(r->command,x->command)&&
        s->owned==r->serial&&rec->serial==r->serial&&rec->epoch==r->epoch&&
        !strcmp(rec->command,x->command)&&!strcmp(s->revision,x->binding.revision)&&
        !strcmp(s->script,x->binding.script)&&!strcmp(s->tasks[r->slot].id,x->binding.task)&&
        x->log->adapter==x->adapter&&x->log->prepared==prepare&&x->log->prepared_context==x;
}
static int unknown(br_schedule_session *x,uint64_t now){
    br_schedule_runner *r=x->runner;
    if(br_schedule_failed(r->scripts[r->script],r->serial,now,0,0)!=1)r->status=BR_RUN_BLOCKED;
    else r->status=BR_RUN_UNKNOWN;
    if(!r->closed){r->closed=1;r->io.close(r->io.context,r->run,r->epoch,now);}
    return r->status;
}
int br_schedule_session_attach(br_schedule_session *x,br_schedule_runner *r,br_flow_session *a,
    br_flow_session_log *j,br_flow_owner_store *o,const br_flow_log_binding *b,
    br_flow_owner_handoff_current previous,void *context){
    if(!x||!r||!a||!j||!o||!b||!previous||r->status!=BR_RUN_WAIT||!r->flow_active||
       r->journal||r->flow||r->script>=r->count||r->slot>=r->scripts[r->script]->count||
       j->phase||j->readonly||j->blocked||j->adapter||a->bound||a->busy||a->session->busy||
       r->op->discovery!=a->session->operation.discovery||a->wire!=o->workspace||
       br_schedule_session_check(r->scripts[r->script],r->slot,a->flow,b,&j->binding,r->command)!=1)return -1;
    memset(x,0,sizeof(*x));x->runner=r;x->adapter=a;x->log=j;x->owner=o;x->binding=*b;
    memcpy(x->command,r->command,37);x->busy=1;
    /* Seed uses the exact original INTENT boundary before use_session changes
     * FLOW identity. It remains a committed prefix if later BOUND fails. */
    if(br_schedule_owner_seed(o,BR_SCHEDULE_OWNER_BYTES,r->scripts[r->script],r->slot,
        a->flow,b,&j->binding,r->command,previous,context)!=1||
       br_flow_session_bind(a,b->run,j->binding.session_run,j->binding.epoch,j->binding.generation)!=1||
       br_flow_session_log_attach(j,a)!=1)goto failed;
    x->attached=1;
    if(br_flow_session_log_set_prepare(j,prepare,x)!=1)goto failed;
    x->busy=0;return 1;
failed:
    j->blocked=1;(void)unknown(x,r->now);x->busy=0;return -1;
}
int br_schedule_session_dispatch(br_schedule_session *x,uint64_t run,uint64_t ticket,uint64_t now,const char *command){
    if(!identity(x,run)||x->unsent_closing||x->runner->status!=BR_RUN_WAIT)return -1;
    x->busy=1;int rc=br_flow_session_dispatch(x->adapter,run,ticket,now,command);
    if(rc<0||rc==BR_FLOW_FAILED||rc==BR_FLOW_CANCELLED)(void)unknown(x,now);
    x->busy=0;return rc;
}
int br_schedule_session_event(br_schedule_session *x,uint64_t run,uint64_t now,int event){
    if(!identity(x,run)||x->runner->status!=BR_RUN_WAIT)return -1;
    br_flow_session *a=x->adapter;br_session *s=a->session;br_flow *f=a->flow;
    if(a->busy||s->busy||!a->active||s->run!=a->session_run||s->epoch!=a->epoch||
       s->count!=a->slot+1||s->generation!=a->generation||s->operation.last_event!=event||
       !event||strcmp(s->tickets[a->slot].command,f->command))return -1;
    x->busy=a->busy=s->busy=1;
    a->io.event(a->io.context,run,a->ticket,&s->operation,event);
    s->busy=0;
    int rc=br_flow_operation(f,run,a->ticket,now,&s->operation,event,&s->operation.discovery->scratch);
    if(rc==BR_FLOW_FAILED||rc==BR_FLOW_CANCELLED)(void)unknown(x,now);
    a->busy=x->busy=0;return rc;
}
int br_schedule_session_poll(br_schedule_session *x,uint64_t run,uint64_t now,int preempt){
    if(!identity(x,run)||x->unsent_closing||x->runner->status!=BR_RUN_WAIT)return -1;
    x->busy=1;int rc=br_flow_session_poll(x->adapter,run,now,preempt);
    if(rc<0||rc==BR_FLOW_FAILED||rc==BR_FLOW_CANCELLED||preempt)(void)unknown(x,now);
    else if(rc==BR_FLOW_DONE)x->runner->status=BR_RUN_RECEIPT;
    x->busy=0;return rc;
}
static void local_identity(br_schedule_session *x,br_result *v){
    memset(v,0,sizeof(*v));v->origin=1;v->epoch=x->log->binding.epoch;
    memcpy(v->command_id,x->command,37);strcpy(v->local.script,x->binding.script);
    strcpy(v->local.revision,x->binding.revision);strcpy(v->local.task,x->binding.task);
    v->local.serial=x->binding.serial;
}
int br_schedule_session_sync(br_schedule_session *x,uint64_t run,br_inbox *q,uint64_t wall,int trusted,br_result *v){
    if(!v||!q||!identity(x,run)||x->receipt||x->runner->status!=BR_RUN_RECEIPT||
       x->adapter->flow->status!=BR_FLOW_DONE||x->log->blocked||x->log->phase!=BR_FLOW_REAPED||
       x->log->index+1!=x->binding.count)return BR_INVALID;
    x->busy=1;local_identity(x,v);size_t n=0;br_flow_session *a=x->adapter;
    br_json *scratch=&a->session->operation.discovery->scratch;int rc=BR_INVALID;
    int output=br_flow_output(a->flow,scratch,a->wire,BR_FLOW_SESSION_WIRE,&n);
    if(output<0)goto done;
    if(output==1){
        if(br_json_parse(scratch,a->wire,n)!=1)goto done;
        br_text t=br_json_text(scratch,1);
        if(t.length>=sizeof(v->text)||memchr(t.data,0,t.length))goto done;
        memcpy(v->text,t.data,t.length);
    }
    rc=br_inbox_sync_receive_policy(q,v,wall,trusted,(int)x->binding.audible,
        x->runner->scripts[x->runner->script]->tasks[x->runner->slot].announce_ttl_s,BR_SPEECH_SCHEDULED);
    if(rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT||rc==BR_DUPLICATE){
        x->receipt=1; /* never recreate after eviction, even on finish failure */
        br_schedule_runner *r=x->runner;
        if(br_schedule_finish(r->scripts[r->script],r->serial,r->epoch,r->command,1)!=1){
            r->status=BR_RUN_BLOCKED;rc=BR_IO;
        }else r->status=BR_RUN_DRAINING;
        if(!r->closed){r->closed=1;r->io.close(r->io.context,run,r->epoch,r->now);}
    }
done:
    memset(v,0,sizeof(*v));memset(scratch,0,sizeof(*scratch));memset(a->wire,0,BR_FLOW_SESSION_WIRE);
    x->busy=0;return rc;
}
int br_schedule_session_drained(br_schedule_session *x,uint64_t run,br_inbox *q,br_result *v){
    if(!v||!q||!identity(x,run)||!x->receipt||x->runner->status!=BR_RUN_DRAINING||
       x->adapter->active||x->adapter->session->live||x->adapter->session->owned)return -1;
    x->busy=1;local_identity(x,v);
    int rc=br_inbox_sync_receipt(q,v,(int)x->binding.audible);
    memset(v,0,sizeof(*v));br_schedule_runner *r=x->runner;
    if(rc==1)rc=r->io.reaped(r->io.context,run,r->epoch)==1?1:-1;
    x->busy=0;return rc;
}
typedef struct {br_schedule_session *x;br_inbox *q;br_result *scratch;} retirement;
static int retirement_ready(void *context,const br_flow_session_log *j){
    retirement *p=context;br_schedule_session *x=p->x;br_schedule_runner *r=x->runner;
    br_schedule *s=r->scripts[r->script];unsigned published=0;
    if(j!=x->log||!j->busy||!x->busy||r->status!=BR_RUN_DRAINING||!x->receipt||
       x->retired||j->binding.run!=x->binding.run||j->binding.epoch!=r->epoch||
       j->binding.source!=BR_LOG_SCHEDULED||j->binding.count!=x->binding.count||
       j->index+1!=x->binding.count||j->phase!=BR_FLOW_REAPED||
       br_schedule_owner_inspect(x->owner,BR_SCHEDULE_OWNER_BYTES,&x->binding,&j->binding,
           x->command,j->steps,x->binding.count,&published)!=1||published!=x->binding.count)return -1;
    local_identity(x,p->scratch);
    int rc=br_inbox_sync_receipt(p->q,p->scratch,(int)x->binding.audible);
    memset(p->scratch,0,sizeof(*p->scratch));
    if(rc!=1||r->io.reaped(r->io.context,r->run,r->epoch)!=1)return -1;
    /* This final read compares ALL persisted scheduler fields, including the
     * original aggregate identity. No mutable cached TERMINAL is sufficient. */
    return br_schedule_terminal_current(s,r->serial,r->epoch,x->command);
}
int br_schedule_session_retire(br_schedule_session *x,uint64_t run,br_inbox *q,br_result *v){
    if(!v||!q||!identity(x,run)||!x->receipt||x->retired||x->runner->status!=BR_RUN_DRAINING||
       x->adapter->active||x->adapter->session->live||x->adapter->session->owned)return -1;
    x->busy=1;retirement p={x,q,v};br_schedule_runner *r=x->runner;
    int rc=br_flow_session_log_retire_scheduled(x->log,retirement_ready,&p);
    if(rc==1){
        if(br_schedule_release(r->scripts[r->script],r->serial,1)!=1){
            r->status=BR_RUN_BLOCKED;rc=-1;
        }else{
            x->retired=1;r->flow_active=0;r->status=BR_RUN_IDLE;
            r->run=r->serial=r->epoch=0;memset(r->command,0,sizeof(r->command));
        }
    }else if(x->log->blocked||x->owner->blocked||r->scripts[r->script]->blocked)r->status=BR_RUN_BLOCKED;
    memset(v,0,sizeof(*v));x->busy=0;return rc;
}
static int unsent_ready(void *p,const br_flow_session_log *j){
    br_schedule_session *x=p;br_schedule_runner *r=x->runner;br_schedule *s=r->scripts[r->script];
    const br_schedule_record *rec=s->records+r->slot;unsigned published=0;
    if(!x->busy||j!=x->log||!j->busy||x->receipt||x->retired||x->deferred||
       j->binding.run!=x->binding.run||j->binding.epoch!=r->epoch||
       j->binding.count!=x->binding.count||
       (rec->phase!=BR_SCHED_INTENT&&!(j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT&&rec->phase==BR_SCHED_PENDING))||
       rec->serial!=r->serial||rec->epoch!=r->epoch||strcmp(rec->command,x->command)||
       br_schedule_owner_inspect(x->owner,BR_SCHEDULE_OWNER_BYTES,&x->binding,&j->binding,x->command,
           NULL,0,&published)!=1||published||br_schedule_state_current(s)!=1)return -1;
    return !x->unsent_closing||(r->closed&&r->io.reaped(r->io.context,r->run,r->epoch)==1)?1:-1;
}
int br_schedule_session_defer_unsent(br_schedule_session *x,uint64_t run,uint64_t now,int auth){
    if(!identity(x,run)||x->receipt||x->retired||x->deferred||x->runner->status!=BR_RUN_WAIT||
       (auth!=0&&auth!=1)||now<x->runner->now||now<x->runner->scripts[x->runner->script]->mono)return -1;
    x->busy=1;br_schedule_runner *r=x->runner;br_schedule *s=r->scripts[r->script];int rc=-1;
    if(!x->unsent_closing){
        if(br_flow_session_log_cancel_unsent(x->log,now,unsent_ready,x)!=1)goto done;
        x->unsent_closing=1;r->now=now;
        if(!r->closed){r->closed=1;r->io.close(r->io.context,run,r->epoch,now);}
    }
    if(x->adapter->session->live||x->adapter->session->owned||x->adapter->active||
       r->io.reaped(r->io.context,run,r->epoch)!=1){rc=0;goto done;}
    if(br_flow_session_log_retire_unsent(x->log,unsent_ready,x)!=1)goto done;
    /* Ordered durable writes, NOT a two-file transaction. Once the distinct
     * tombstone is published, any ambiguity keeps the original claim. */
    if(br_flow_session_log_inspect(x->log,unsent_ready,x)!=1||
       br_schedule_failed(s,r->serial,now,1,auth)!=1||br_flow_session_log_inspect(x->log,unsent_ready,x)!=1||
       r->io.reaped(r->io.context,run,r->epoch)!=1||br_schedule_release(s,r->serial,1)!=1){
        x->log->blocked=1;r->status=BR_RUN_BLOCKED;goto done;
    }
    x->deferred=1;r->flow_active=0;r->status=BR_RUN_IDLE;
    r->run=r->serial=r->epoch=0;memset(r->command,0,sizeof r->command);rc=1;
done:
    if(x->log->blocked||x->owner->blocked||s->blocked)r->status=BR_RUN_BLOCKED;
    x->busy=0;return rc;
}
typedef struct {br_schedule_session *x;br_inbox *inbox;br_result *scratch;uint64_t wall;int trusted;} failed_event;
static int failure_owner(br_schedule_session *x,const br_flow_session_log *j){
    unsigned count=0;
    return j==x->log&&x->busy&&j->busy&&j->binding.run==x->binding.run&&
        br_schedule_owner_inspect(x->owner,BR_SCHEDULE_OWNER_BYTES,&x->binding,&j->binding,x->command,
            j->steps,j->index+1,&count)==1&&count==j->index+1;
}
static int failure_publish(void *p,const br_flow_session_log *j){
    failed_event *e=p;br_schedule_session *x=e->x;br_schedule_runner *r=x->runner;br_result *v=e->scratch;
    br_operation *op=&x->adapter->session->operation;const br_json *body=&op->response;
    unsigned message=br_json_field(body,1,"msg");br_text text=br_json_text(body,message);
    if(x->failed||(j->phase==BR_FLOW_PREPARE?x->receipt:j->phase!=BR_FLOW_SESSION_LOG_SCHEDULED_FAILED||!x->receipt)||!failure_owner(x,j)||
       (message&&br_json_kind(body,message)!=BR_J_STRING)||text.length>=sizeof v->text||
       (text.length&&memchr(text.data,0,text.length))||strcmp(op->error,j->failure_code)||
       r->scripts[r->script]->records[r->slot].phase!=(j->phase==BR_FLOW_PREPARE?BR_SCHED_INTENT:BR_SCHED_TERMINAL)||
       br_schedule_state_current(r->scripts[r->script])!=1)return -1;
    if(j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_FAILED){
        local_identity(x,v);const br_result *stored=NULL;
        return br_inbox_sync_result(e->inbox,v,(int)x->binding.audible,&stored)==1&&stored&&
            !strcmp(stored->code,j->failure_code)&&strlen(stored->text)==text.length&&
            (!text.length||!memcmp(stored->text,text.data,text.length))?1:-1;
    }
    local_identity(x,v);strcpy(v->code,op->error);if(text.length)memcpy(v->text,text.data,text.length);
    int rc=br_inbox_sync_receive_policy(e->inbox,v,e->wall,e->trusted,(int)x->binding.audible,
        r->scripts[r->script]->tasks[r->slot].announce_ttl_s,BR_SPEECH_SCHEDULED);
    memset(v->code,0,sizeof v->code);memset(v->text,0,sizeof v->text);
    if(rc!=BR_ACCEPTED&&rc!=BR_ACCEPTED_SILENT&&rc!=BR_DUPLICATE)return -1;
    x->receipt=1; /* original durable aggregate is never recreated after eviction */
    const br_result *stored=NULL;
    if(br_inbox_sync_result(e->inbox,v,(int)x->binding.audible,&stored)!=1||!stored||
       strcmp(stored->code,j->failure_code)||strlen(stored->text)!=text.length||(text.length&&memcmp(stored->text,text.data,text.length))||
       !failure_owner(x,j)||br_schedule_state_current(r->scripts[r->script])!=1||
       br_schedule_finish(r->scripts[r->script],r->serial,r->epoch,x->command,1)!=1||
       br_schedule_terminal_current(r->scripts[r->script],r->serial,r->epoch,x->command)!=1)return -1;
    return 1;
}
int br_schedule_session_failure(br_schedule_session *x,uint64_t run,uint64_t now,br_inbox *q,uint64_t wall,int trusted,br_result *scratch){
    if(!q||!scratch||!identity(x,run)||x->runner->status!=BR_RUN_WAIT||x->receipt||x->failed||x->unsent_closing||
       !x->adapter->active||x->adapter->session->operation.last_event!=BR_OP_ERROR||now<x->adapter->flow->now_ms)return -1;
    x->busy=1;failed_event e={x,q,scratch,wall,trusted};br_schedule_runner *r=x->runner;
    int rc=br_flow_session_log_record_failure(x->log,failure_publish,&e);
    /* Consume the same original terminal exactly once. Its normal cancel and
     * close preserve every ticket until the actual original reap callback. */
    int status=br_flow_session_event(x->adapter,run,x->adapter->ticket,now,BR_OP_ERROR);
    if(rc==1&&status==BR_FLOW_DRAINING){x->failed=1;r->status=BR_RUN_DRAINING;r->now=now;}
    else{
        x->log->blocked=1;
        r->status=br_schedule_failed(r->scripts[r->script],r->serial,now,0,0)==1?BR_RUN_UNKNOWN:BR_RUN_BLOCKED;rc=-1;
    }
    /* The profile's original event borrow is still active. Do not reenter
     * transport close here; its original ERROR cleanup closes after callback.
     * The outer event-loop poll requests global close only after pump returns. */
    memset(scratch,0,sizeof(*scratch));x->busy=0;return rc;
}
int br_schedule_session_failure_poll(br_schedule_session *x,uint64_t run,uint64_t now){
    if(!identity(x,run)||!x->failed||!x->receipt||x->retired||x->runner->status!=BR_RUN_DRAINING||
       x->log->blocked||x->log->phase!=BR_FLOW_SESSION_LOG_SCHEDULED_FAILED)return -1;
    x->busy=1;br_schedule_runner *r=x->runner;
    if(!r->closed){r->closed=1;r->io.close(r->io.context,run,r->epoch,now);}
    int rc=br_flow_session_poll(x->adapter,run,now,0);
    if(rc!=BR_FLOW_DRAINING&&rc!=BR_FLOW_FAILED){x->log->blocked=1;x->runner->status=BR_RUN_BLOCKED;rc=-1;}
    x->busy=0;return rc;
}
static int failure_retired_ready(void *p,const br_flow_session_log *j){
    failed_event *e=p;br_schedule_session *x=e->x;br_schedule_runner *r=x->runner;const br_result *stored=NULL;
    if(!x->failed||!x->receipt||x->retired||r->status!=BR_RUN_DRAINING||
       (j->phase!=BR_FLOW_SESSION_LOG_SCHEDULED_FAILED&&j->phase!=BR_FLOW_SESSION_LOG_FAILURE_RETIRED)||!failure_owner(x,j))return -1;
    local_identity(x,e->scratch);
    int rc=br_inbox_sync_result(e->inbox,e->scratch,(int)x->binding.audible,&stored);
    memset(e->scratch,0,sizeof(*e->scratch));
    return rc==1&&stored&&!strcmp(stored->code,j->failure_code)&&r->io.reaped(r->io.context,r->run,r->epoch)==1&&
        br_schedule_terminal_current(r->scripts[r->script],r->serial,r->epoch,x->command)==1?1:-1;
}
int br_schedule_session_retire_failure(br_schedule_session *x,uint64_t run,br_inbox *q,br_result *scratch){
    if(!q||!scratch||!identity(x,run)||!x->failed||!x->receipt||x->retired||
       x->runner->status!=BR_RUN_DRAINING)return -1;
    x->busy=1;failed_event e={x,q,scratch,0,0};br_schedule_runner *r=x->runner;
    int rc=br_flow_session_log_retire_failure(x->log,failure_retired_ready,&e);
    if(rc==1){
        if(r->io.reaped(r->io.context,run,r->epoch)!=1||
           br_schedule_terminal_current(r->scripts[r->script],r->serial,r->epoch,x->command)!=1||
           br_schedule_release(r->scripts[r->script],r->serial,1)!=1){x->log->blocked=1;r->status=BR_RUN_BLOCKED;rc=-1;}
        else{x->retired=1;r->flow_active=0;r->status=BR_RUN_IDLE;r->run=r->serial=r->epoch=0;memset(r->command,0,sizeof r->command);}
    }else if(x->log->blocked||x->owner->blocked||r->scripts[r->script]->blocked)r->status=BR_RUN_BLOCKED;
    memset(scratch,0,sizeof(*scratch));x->busy=0;return rc;
}
typedef struct {
    br_schedule_session *old;br_inbox *inbox;br_result *scratch;
    br_flow_session_log_closed closed;void *context;int kind;
    unsigned failed_count;char failure_code[40];
} scheduled_predecessor;
static br_schedule *retired_schedule(br_schedule_session *x){
    if(!x||!x->runner)return NULL;
    for(size_t i=0;i<x->runner->count;i++){
        br_schedule *s=x->runner->scripts[i];
        if(!strcmp(s->script,x->binding.script)&&!strcmp(s->revision,x->binding.revision))return s;
    }return NULL;
}
static int predecessor_closed(void *p,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,unsigned count){
    scheduled_predecessor *v=p;br_schedule_session *x=v->old;br_result *id=v->scratch;
    if(!x->busy||(v->kind==1?(!x->deferred||x->retired||x->receipt||count):
       (!x->retired||!x->receipt||(v->kind==2?(!x->failed||count!=v->failed_count):x->failed||count!=x->binding.count)))||
       b->source!=BR_LOG_SCHEDULED||b->ctl_run||b->cycle||b->job||b->run!=x->binding.run||b->count!=x->binding.count||
       strcmp(b->script,x->binding.script)||strcmp(b->revision,x->binding.revision)||
       strcmp(b->definition_sha256,x->binding.definition_sha256)||strcmp(b->inputs_sha256,x->binding.inputs_sha256))return -1;
    if(v->kind==1)return x->runner->io.reaped(x->runner->io.context,b->run,b->epoch)==1&&
        v->closed(v->context,b,steps,0)==1?1:-1;
    memset(id,0,sizeof(*id));id->origin=1;id->epoch=b->epoch;strcpy(id->command_id,x->command);
    strcpy(id->local.script,x->binding.script);strcpy(id->local.revision,x->binding.revision);
    strcpy(id->local.task,x->binding.task);id->local.serial=x->binding.serial;
    int rc;
    if(v->kind==2){
        const br_result *stored=NULL;
        rc=br_inbox_sync_result(v->inbox,id,(int)x->binding.audible,&stored);
        if(rc!=1||!stored||!v->failure_code[0]||strcmp(stored->code,v->failure_code))rc=-1;
    }else rc=br_inbox_sync_receipt(v->inbox,id,(int)x->binding.audible);
    memset(id,0,sizeof(*id));
    return rc==1&&x->runner->io.reaped(x->runner->io.context,b->run,b->epoch)==1&&
        v->closed(v->context,b,steps,count)==1?1:-1;
}
static int predecessor_image(void *p,const br_flow_session_log *j){
    scheduled_predecessor *v=p;br_schedule_session *x=v->old;unsigned count=0;
    unsigned expected=v->kind==1?0:v->kind==2?j->index+1:j->binding.count;
    return j==x->log&&j->phase==(v->kind==2?BR_FLOW_SESSION_LOG_FAILURE_RETIRED:v->kind==1?BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT:BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED)&&j->binding.run==x->binding.run&&
        br_schedule_owner_inspect(x->owner,BR_SCHEDULE_OWNER_BYTES,&x->binding,&j->binding,x->command,
            j->steps,expected,&count)==1&&count==expected?1:-1;
}
int br_schedule_session_retired_inspect(br_schedule_session *x,br_inbox *q,br_result *scratch,
    br_flow_session_log_closed closed,void *context,int terminal){
    if(!x||x->busy||!x->attached||!x->retired||!x->receipt||x->failed||!q||!scratch||!closed||
       (terminal!=0&&terminal!=1)||!x->log||x->log->readonly)return -1;
    br_schedule *s=retired_schedule(x);
    if(!s||x->runner->run||x->runner->status!=BR_RUN_IDLE||x->runner->flow_active||
       (terminal&&br_schedule_retired_current(s,x->binding.serial,x->log->binding.epoch,x->command)!=1))return -1;
    x->busy=1;scheduled_predecessor proof={.old=x,.inbox=q,.scratch=scratch,.closed=closed,.context=context,.kind=0};
    int rc=br_flow_session_log_inspect_scheduled_retired(x->log,predecessor_image,predecessor_closed,&proof);
    if(rc==1&&terminal)rc=br_schedule_retired_current(s,x->binding.serial,x->log->binding.epoch,x->command);
    memset(scratch,0,sizeof(*scratch));x->busy=0;return rc;
}
int br_schedule_session_deferred_inspect(br_schedule_session *x,br_flow_session_log_closed closed,void *context,int pending){
    if(!x||x->busy||!x->attached||!x->deferred||x->retired||x->receipt||!closed||
       (pending!=0&&pending!=1)||!x->log||x->log->readonly)return -1;
    br_schedule *s=retired_schedule(x);
    if(!s||x->runner->run||x->runner->status!=BR_RUN_IDLE||x->runner->flow_active||
       (pending&&br_schedule_deferred_current(s,x->binding.serial,x->log->binding.epoch,x->command)!=1))return -1;
    x->busy=1;scheduled_predecessor proof={.old=x,.closed=closed,.context=context,.kind=1};
    int rc=br_flow_session_log_inspect_unsent(x->log,predecessor_image,predecessor_closed,&proof);
    if(rc==1&&pending)rc=br_schedule_deferred_current(s,x->binding.serial,x->log->binding.epoch,x->command);
    x->busy=0;return rc;
}
int br_schedule_session_failure_retired_inspect(br_schedule_session *x,br_inbox *q,br_result *scratch,
    br_flow_session_log_closed closed,void *context,int terminal){
    if(!x||x->busy||!x->attached||!x->retired||!x->receipt||!x->failed||x->deferred||!q||!scratch||!closed||
       (terminal!=0&&terminal!=1)||!x->log||x->log->readonly)return -1;
    br_schedule *s=retired_schedule(x);
    if(!s||x->runner->run||x->runner->status!=BR_RUN_IDLE||x->runner->flow_active||
       (terminal&&br_schedule_retired_current(s,x->binding.serial,x->log->binding.epoch,x->command)!=1))return -1;
    x->busy=1;scheduled_predecessor proof={.old=x,.inbox=q,.scratch=scratch,.closed=closed,.context=context,
        .kind=2,.failed_count=x->log->index+1};
    memcpy(proof.failure_code,x->log->failure_code,sizeof proof.failure_code);
    int rc=br_flow_session_log_inspect_failure_retired(x->log,predecessor_image,predecessor_closed,&proof);
    if(rc==1&&terminal)rc=br_schedule_retired_current(s,x->binding.serial,x->log->binding.epoch,x->command);
    memset(scratch,0,sizeof(*scratch));memset(proof.failure_code,0,sizeof proof.failure_code);x->busy=0;return rc;
}
static int session_successor(br_schedule_session *x,br_schedule_session *old,
    br_schedule_runner *r,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *o,
    const br_flow_log_binding *b,const br_flow_session_log_binding *next,br_inbox *q,br_result *scratch,
    br_flow_owner_handoff_current previous,void *previous_context,br_flow_session_log_closed closed,void *closed_context,int kind){
    if(!x||!old||x==old||x->attached||x->busy||old->busy||
       (kind==1?(!old->deferred||old->retired||old->receipt):(!old->retired||!old->receipt||old->failed!=(kind==2)))||
       old->log!=j||old->owner!=o||old->runner!=r||!r||!a||!j||!o||!b||!next||(kind!=1&&(!q||!scratch))||!previous||!closed||
       r->status!=BR_RUN_WAIT||!r->flow_active||r->journal||r->flow||r->script>=r->count||
       r->slot>=r->scripts[r->script]->count||j->readonly||j->blocked||j->busy||a->bound||a->busy||
       !a->session||a->session->busy||a->wire!=o->workspace||r->op!=&a->session->operation||
       br_schedule_session_check(r->scripts[r->script],r->slot,a->flow,b,next,r->command)!=1)return -1;
    memset(x,0,sizeof(*x));x->runner=r;x->adapter=a;x->log=j;x->owner=o;x->binding=*b;strcpy(x->command,r->command);
    x->busy=old->busy=1;scheduled_predecessor proof={.old=old,.inbox=q,.scratch=scratch,.closed=closed,.context=closed_context,
        .kind=kind,.failed_count=j->index+1};int rc=-1;
    if(kind==2)memcpy(proof.failure_code,j->failure_code,sizeof proof.failure_code);
    if(br_flow_session_bind(a,b->run,next->session_run,next->epoch,next->generation)!=1)goto bad;
    rc=(kind==2?br_schedule_owner_after_failure:kind==1?br_schedule_owner_retry:br_schedule_owner_turnover)(o,j,a,&old->binding,old->command,b,next,x->command,previous,previous_context,
        predecessor_closed,&proof,prepare,x,r->wire,sizeof r->wire);
    if(rc!=1)goto bad;
    x->attached=1;rc=1;goto done;
bad:j->blocked=1;(void)unknown(x,r->now);rc=-1;
done:if(scratch)memset(scratch,0,sizeof(*scratch));memset(proof.failure_code,0,sizeof proof.failure_code);old->busy=x->busy=0;return rc;
}
int br_schedule_session_turnover(br_schedule_session *x,br_schedule_session *old,
    br_schedule_runner *r,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *o,
    const br_flow_log_binding *b,const br_flow_session_log_binding *next,br_inbox *q,br_result *scratch,
    br_flow_owner_handoff_current previous,void *previous_context,br_flow_session_log_closed closed,void *closed_context){
    return session_successor(x,old,r,a,j,o,b,next,q,scratch,previous,previous_context,closed,closed_context,0);
}
int br_schedule_session_retry(br_schedule_session *x,br_schedule_session *old,
    br_schedule_runner *r,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *o,
    const br_flow_log_binding *b,const br_flow_session_log_binding *next,
    br_flow_owner_handoff_current previous,void *previous_context,br_flow_session_log_closed closed,void *closed_context){
    return session_successor(x,old,r,a,j,o,b,next,NULL,NULL,previous,previous_context,closed,closed_context,1);
}
int br_schedule_session_after_failure(br_schedule_session *x,br_schedule_session *old,
    br_schedule_runner *r,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *o,
    const br_flow_log_binding *b,const br_flow_session_log_binding *next,br_inbox *q,br_result *scratch,
    br_flow_owner_handoff_current previous,void *previous_context,br_flow_session_log_closed closed,void *closed_context){
    return session_successor(x,old,r,a,j,o,b,next,q,scratch,previous,previous_context,closed,closed_context,2);
}

typedef struct {
    br_schedule_session *x;br_result *identity;unsigned char response[32],caps[32];int rc;
} schedule_association;
static int association_current(void *p){
    schedule_association *v=p;br_schedule_session *x=v->x;br_schedule_runner *r=x->runner;
    br_flow_session_log *j=x->log;br_flow_session *a=x->adapter;br_session *s=a->session;
    br_operation *op=&s->operation;br_discovery *d=op->discovery;br_flow *f=a->flow;
    if(!x->busy||!x->attached||x->receipt||x->failed||x->retired||x->unsent_closing||
       r->status!=BR_RUN_WAIT||!r->flow_active||r->script>=r->count||r->slot>=r->scripts[r->script]->count||
       r->run!=x->binding.run||r->serial!=x->binding.serial||r->epoch!=j->binding.epoch||strcmp(r->command,x->command)||
       !j->busy||j->readonly||j->blocked||j->phase!=BR_FLOW_PREPARE||j->index>=j->binding.count||
       j->adapter!=a||!a->busy||!a->bound||!a->active||!s->busy||!s->live||!s->owned||s->terminal||s->ended||
       s->run!=j->binding.session_run||s->epoch!=j->binding.epoch||s->generation!=j->binding.generation||
       a->run!=j->binding.run||a->epoch!=j->binding.epoch||a->generation!=j->binding.generation||
       s->count!=a->slot+1||s->count>BR_SESSION_LIMIT||s->count<j->index+1||
       a->ticket!=j->steps[j->index].ticket||f->status!=BR_FLOW_WAIT||f->index!=j->index||f->ticket!=a->ticket||
       f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j||
       strcmp(s->script,j->binding.script)||strcmp(f->command,j->steps[j->index].command)||
       op->last_event!=BR_OP_TASK||!op->acked||!op->pending||op->completed||op->epoch!=j->binding.epoch||
       v->identity->origin||v->identity->code[0]||v->identity->text[0]||
       !br_json_is(&op->response,br_json_field(&op->response,1,"task_id"),v->identity->task_id)||
       !br_json_is(&op->response,br_json_field(&op->response,1,"app_version"),v->identity->app_version)||
       !br_json_is(&op->response,br_json_field(&op->response,1,"hook_version"),v->identity->hook_version)||
       strcmp(op->command_id,j->steps[j->index].command)||strcmp(op->task_id,v->identity->task_id)||
       v->identity->epoch!=j->binding.epoch||strcmp(v->identity->command_id,op->command_id)||
       !d||d->epoch!=j->binding.epoch||d->phase!=BR_DISC_READY||d->describing||
       !br_caps_has_feature(&d->caps,j->binding.epoch,"session.command_id"))return -1;
    for(unsigned i=0;i<=j->index;i++){
        const br_session_ticket *t=s->tickets+s->count-(j->index+1)+i;
        if(t->generation!=j->binding.generation||strcmp(t->command,j->steps[i].command))return -1;
    }
    const br_session_ticket *ticket=s->tickets+a->slot;
    if(!ticket->grant_seen||strcmp(ticket->task,op->task_id)||ticket->result_type)return -1;
    br_schedule *schedule=r->scripts[r->script];const br_schedule_record *rec=schedule->records+r->slot;
    if(schedule->owned!=r->serial||rec->phase!=BR_SCHED_INTENT||rec->serial!=r->serial||rec->epoch!=r->epoch||
       strcmp(rec->command,x->command)||strcmp(schedule->script,x->binding.script)||
       strcmp(schedule->revision,x->binding.revision)||strcmp(schedule->tasks[r->slot].id,x->binding.task)||
       br_schedule_state_current(schedule)!=1)return -1;
    unsigned char hash[32];br_result_fingerprint(&op->response,hash);int same=!memcmp(hash,v->response,sizeof hash);
    br_result_fingerprint(&d->caps.document,hash);same=same&&!memcmp(hash,v->caps,sizeof hash);
    memset(hash,0,sizeof hash);return same?1:-1;
}
static int associate_visit(void *p,const br_flow_session_log *j){
    schedule_association *v=p;br_schedule_session *x=v->x;
    if(j!=x->log||association_current(v)!=1)return -1;
    v->rc=br_schedule_owner_associate(x->owner,x->log,&x->binding,x->command,j->index,v->identity,
        association_current,v,x->runner->wire,sizeof x->runner->wire);
    return v->rc==1&&association_current(v)==1?1:-1;
}
static int associate_after(void *p,const br_flow_session_log *j){
    schedule_association *v=p;br_schedule_session *x=v->x;
    return j==x->log&&association_current(v)==1&&
        br_schedule_owner_associated(x->owner,&x->binding,&j->binding,x->command,j->steps,j->index+1,j->index,v->identity)==1&&
        association_current(v)==1?1:-1;
}
int br_schedule_session_associate_task(br_schedule_session *x,uint64_t run,br_result *scratch){
    if(!scratch||!identity(x,run)||x->receipt||x->failed||x->retired||x->unsent_closing||
       x->runner->status!=BR_RUN_WAIT||x->adapter->busy||!x->adapter->active||
       x->adapter->session->busy||x->log->phase!=BR_FLOW_PREPARE||x->log->readonly||x->log->blocked)return BR_INVALID;
    memset(scratch,0,sizeof(*scratch));br_operation *op=&x->adapter->session->operation;
    if(op->last_event!=BR_OP_TASK||!op->acked||!br_delivery_decode(&op->response,1,scratch)||
       scratch->epoch!=x->log->binding.epoch||strcmp(scratch->command_id,op->command_id)||
       strcmp(scratch->task_id,op->task_id)){memset(scratch,0,sizeof(*scratch));return BR_INVALID;}
    schedule_association v={.x=x,.identity=scratch,.rc=BR_INVALID};
    br_result_fingerprint(&op->response,v.response);br_result_fingerprint(&op->discovery->caps.document,v.caps);
    memset(scratch->code,0,sizeof scratch->code);memset(scratch->text,0,sizeof scratch->text);x->busy=1;
    int rc=br_flow_session_log_inspect(x->log,associate_visit,&v);
    if(rc==1)rc=br_flow_session_log_inspect(x->log,associate_after,&v);
    if(rc!=1&&v.rc==1)x->owner->blocked=x->log->blocked=1;
    memset(scratch,0,sizeof(*scratch));memset(&v,0,sizeof v);x->busy=0;return rc==1?1:BR_INVALID;
}
