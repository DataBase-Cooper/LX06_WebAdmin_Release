#include "schedule_run.h"
#include <string.h>
#include <stdlib.h>
_Static_assert(sizeof(br_schedule_runner)<12*1024,"bounded scheduler adapter");
static int active(const br_schedule_record *r){return r->phase>=BR_SCHED_INTENT&&r->phase<=BR_SCHED_UNKNOWN;}
static br_schedule *script(br_schedule_runner *r){return r->scripts[r->script];}
static br_schedule_record *record(br_schedule_runner *r){return script(r)->records+r->slot;}
typedef struct { const void *p;size_t n; } span;
static int disjoint(span a,span b){
    uintptr_t x=(uintptr_t)a.p,y=(uintptr_t)b.p;
    if(!a.n||!b.n)return 1;
    if(!x||!y||a.n>UINTPTR_MAX-x||b.n>UINTPTR_MAX-y)return 0;
    return x+a.n<=y||y+b.n<=x;
}
static int flow_memory(br_schedule_runner *r,size_t base,size_t *total){
    if(!r->flow&&!r->flow_scratch&&!r->journal){*total=base;return 1;}
    if(!r->flow||!r->flow_scratch||!r->journal)return 0;
    br_flow_journal *j=r->journal;
    if(!j->store||j->store->schedule!=2||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||j->busy||
       j->store!=r->journal_store||j->workspace!=r->journal_workspace||
       j->capacity!=r->journal_capacity||j->scratch!=r->journal_scratch)return 0;
    const span extra[]={ {r->flow,sizeof(*r->flow)},{r->flow_scratch,sizeof(*r->flow_scratch)},
        {j,sizeof(*j)},{j->store,sizeof(*j->store)},{j->workspace,j->capacity},{j->scratch,sizeof(*j->scratch)} };
    size_t bytes=base;
    for(unsigned i=0;i<sizeof(extra)/sizeof(*extra);i++){
        if(extra[i].n>BR_RUN_FLOW_BYTES-bytes||!disjoint(extra[i],(span){r,sizeof(*r)})||
           !disjoint(extra[i],(span){r->op,sizeof(*r->op)})||
           (r->op->discovery&&!disjoint(extra[i],(span){r->op->discovery,sizeof(*r->op->discovery)})))return 0;
        bytes+=extra[i].n;
        for(unsigned k=0;k<i;k++)if(!disjoint(extra[i],extra[k]))return 0;
        for(size_t k=0;k<r->count;k++){
            br_schedule *s=r->scripts[k];
            const span ss[]={ {s,sizeof(*s)},{s->records,s->capacity*sizeof(*s->records)},
                {s->history,s->history_capacity*sizeof(*s->history)},
                {s->tasks,s->count*sizeof(*s->tasks)},{s->workspace,sizeof(*s->workspace)} };
            for(unsigned t=0;t<sizeof(ss)/sizeof(*ss);t++)if(!disjoint(extra[i],ss[t]))return 0;
        }
    }
    *total=bytes;return 1;
}
static int registry(br_schedule_runner *r,int *busy){
    if(!r->op||r->count>32)return 0;
    size_t tasks=0,memory=sizeof(*r);*busy=0;
    for(size_t i=0;i<r->count;i++){
        br_schedule *s=r->scripts[i];
        if(!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy)return 0;
        tasks+=s->count;if(tasks>32)return 0;
        memory+=sizeof(*s)+s->capacity*sizeof(*s->records)+s->history_capacity*sizeof(*s->history);
        int shared=0;
        for(size_t j=0;j<i;j++){
            if(!br_schedule_storage_separate(s,r->scripts[j])||!strcmp(s->script,r->scripts[j]->script))return 0;
            if(s->workspace==r->scripts[j]->workspace)shared=1;
        }
        if(!shared)memory+=sizeof(*s->workspace);
        if(memory>BR_RUN_BASE_BYTES)return 0;
        if(s->owned||!s->reconciled)*busy=1;
        for(size_t j=0;j<s->count;j++)if(active(s->records+j))*busy=1;
    }
    size_t total;if(!flow_memory(r,memory,&total))return 0;
    if(r->journal){
        if(r->journal->blocked||r->journal->store->poisoned||r->journal->phase>BR_FLOW_LOG_ERROR_RETIRED)return 0;
        if(r->journal->phase&&r->journal->phase!=BR_FLOW_LOG_RETIRED&&r->journal->phase!=BR_FLOW_LOG_UNSENT&&
           r->journal->phase!=BR_FLOW_LOG_ERROR_RETIRED)*busy=1;
    }
    return 1;
}
int br_schedule_runner_memory(br_schedule_runner *r,size_t *base,size_t *total){
    if(!r||!base||!total||base==total||!r->op||r->count>32)return -1;
    int busy;if(!registry(r,&busy))return -1;
    size_t bytes=sizeof(*r),all;
    for(size_t i=0;i<r->count;i++){
        br_schedule *s=r->scripts[i];
        bytes+=sizeof(*s)+s->capacity*sizeof(*s->records)+s->history_capacity*sizeof(*s->history);
        int shared=0;for(size_t k=0;k<i;k++)if(s->workspace==r->scripts[k]->workspace)shared=1;
        if(!shared)bytes+=sizeof(*s->workspace);
    }
    if(!flow_memory(r,bytes,&all))return -1;
    *base=bytes;*total=all;return 1;
}
int br_schedule_runner_attach_flow(br_schedule_runner *r,br_flow *f,br_json *scratch,br_flow_journal *j){
    if(!r||r->status!=BR_RUN_IDLE||r->candidate||r->run||r->flow||r->flow_scratch||r->journal||!f||!scratch||!j)return -1;
    int busy;if(!registry(r,&busy))return -1;
    r->flow=f;r->flow_scratch=scratch;r->journal=j;
    r->journal_scratch=j->scratch;r->journal_store=j->store;
    r->journal_workspace=j->workspace;r->journal_capacity=j->capacity;
    if(!registry(r,&busy)){
        r->flow=NULL;r->flow_scratch=NULL;r->journal=NULL;r->journal_scratch=NULL;
        r->journal_store=NULL;r->journal_workspace=NULL;r->journal_capacity=0;return -1;
    }
    return 1;
}
int br_schedule_runner_init(br_schedule_runner *r,br_schedule_run_io io,br_operation *op,br_schedule *const *ss,size_t n){
    if(!r)return -1;
    memset(r,0,sizeof(*r));r->status=BR_RUN_BLOCKED;
    if(!io.acquire||!io.send||!io.close||!io.reaped||!op||n>32||(n&&!ss))return -1;
    r->io=io;r->op=op;r->count=n;
    for(size_t i=0;i<n;i++)r->scripts[i]=ss[i];
    int busy;if(!registry(r,&busy))return -1;
    r->status=BR_RUN_IDLE;return 1;
}
int br_schedule_runner_poll(br_schedule_runner *r,int trusted,int64_t wall,uint64_t mono,int idle,br_schedule_candidate *out){
    if(!r||!out||(r->journal&&r->journal->busy)||r->status==BR_RUN_BLOCKED||trusted<0||trusted>1||idle<0||idle>1)return -1;
    r->candidate=0;
    if(r->status!=BR_RUN_IDLE)return 0;
    if(r->sampled&&mono<r->now){r->status=BR_RUN_BLOCKED;return -1;}
    r->sampled=1;r->now=mono;
    int busy;if(!registry(r,&busy)){r->status=BR_RUN_BLOCKED;return -1;}
    /* Preserve the original TERMINAL identity until journal retirement has
     * checked it. Even an idle=false schedule poll replans completed slots. */
    if(r->journal&&r->journal->phase&&r->journal->phase!=BR_FLOW_LOG_RETIRED&&r->journal->phase!=BR_FLOW_LOG_UNSENT&&
       r->journal->phase!=BR_FLOW_LOG_ERROR_RETIRED)return 0;
    int found=0;int64_t due=0;br_schedule_candidate chosen={0,0};
    for(unsigned i=0;i<r->count;i++){
        br_schedule *s=r->scripts[i];
        int rc=br_schedule_poll(s,trusted,wall,mono,idle&&!busy);
        if(rc<0)return -1;
        if(rc>0){
            unsigned slot=(unsigned)rc-1;
            if(!found||s->records[slot].due<due){found=1;due=s->records[slot].due;chosen=(br_schedule_candidate){i,slot};}
        }
    }
    if(!found||busy)return 0;
    r->next=chosen;r->candidate=1;*out=chosen;return 1;
}
static void close_owned(br_schedule_runner *r){
    if(!r->closed){r->closed=1;r->io.close(r->io.context,r->run,r->epoch,r->now);}
}
static int blocked(br_schedule_runner *r){r->status=BR_RUN_BLOCKED;close_owned(r);return r->status;}
static int fail(br_schedule_runner *r,int unsent,int auth){
    if(br_schedule_failed(script(r),r->serial,r->now,unsent,auth)!=1)return blocked(r);
    r->status=unsent?BR_RUN_DRAINING:BR_RUN_UNKNOWN;close_owned(r);return r->status;
}
static int expired(br_schedule_runner *r){r->timed_out=1;return fail(r,0,0);}
static void own(br_schedule_runner *r,uint64_t run,unsigned i,unsigned slot,uint64_t mono){
    br_schedule_record *rec=r->scripts[i]->records+slot;
    r->run=r->last_run=run;r->script=i;r->slot=slot;r->serial=rec->serial;r->epoch=rec->epoch;
    memcpy(r->command,rec->command,37);r->now=mono;r->closed=r->oneway=r->acked=0;r->candidate=0;
    r->started=mono;r->timeout=0;r->timed_out=0;
}
int br_schedule_runner_claim_flow(br_schedule_runner *r,uint64_t run,br_schedule_candidate c,uint64_t epoch,
                                  const char *uuid,uint64_t mono){
    if(!r||r->flow_active||r->status!=BR_RUN_IDLE||!r->candidate||!r->flow||!r->journal||
       !run||run<=r->last_run||!epoch||mono!=r->now||c.script!=r->next.script||c.slot!=r->next.slot||
       r->flow->run!=run||r->flow->status!=BR_FLOW_NEXT||r->flow->index||r->flow->ticket||r->flow->checkpoint)return -1;
    int busy;if(!registry(r,&busy)||busy)return -1;
    br_discovery *d=r->op->discovery;
    if(!d||d->phase!=BR_DISC_READY||d->epoch!=epoch||!d->caps.ready||d->caps.connection!=epoch)return -1;
    int a=r->io.acquire(r->io.context,run);
    if(a==0)return BR_RUN_IDLE;
    if(a!=1){r->run=r->last_run=run;r->epoch=epoch;r->closed=0;return blocked(r);}
    int rc=br_schedule_claim(r->scripts[c.script],c.slot,mono,epoch,uuid);
    own(r,run,c.script,c.slot,mono);r->epoch=epoch;r->flow_active=1;
    if(rc!=1)return blocked(r);
    r->status=BR_RUN_WAIT;return r->status;
}
int br_schedule_runner_claim_session(br_schedule_runner *r,br_flow *f,uint64_t run,
    br_schedule_candidate c,uint64_t epoch,const char *uuid,uint64_t mono){
    if(!r||!f||r->flow||r->journal||r->flow_active||r->status!=BR_RUN_IDLE||!r->candidate||
       !run||run<=r->last_run||!epoch||mono!=r->now||c.script!=r->next.script||c.slot!=r->next.slot||
       c.script>=r->count||c.slot>=r->scripts[c.script]->count||f->run!=run||
       f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->checkpoint||f->session_epoch)return -1;
    int busy;if(!registry(r,&busy)||busy)return -1;
    br_discovery *d=r->op->discovery;
    if(!d||d->phase!=BR_DISC_READY||d->epoch!=epoch||!d->caps.ready||d->caps.connection!=epoch)return -1;
    int a=r->io.acquire(r->io.context,run);
    if(a==0)return BR_RUN_IDLE;
    if(a!=1){r->run=r->last_run=run;r->epoch=epoch;r->closed=0;return blocked(r);}
    /* Acquire may invalidate the sampled activity/connection. Do not claim
     * or send under a successor connection, but retain acquired obligations. */
    if(d->phase!=BR_DISC_READY||d->epoch!=epoch||!d->caps.ready||d->caps.connection!=epoch){
        r->run=r->last_run=run;r->epoch=epoch;r->closed=0;return blocked(r);
    }
    int rc=br_schedule_claim(r->scripts[c.script],c.slot,mono,epoch,uuid);
    own(r,run,c.script,c.slot,mono);r->epoch=epoch;r->flow_active=1;
    if(rc!=1)return blocked(r);
    r->status=BR_RUN_WAIT;return r->status;
}
int br_schedule_runner_start(br_schedule_runner *r,uint64_t run,br_schedule_candidate c,uint64_t epoch,
                            const char *uuid,uint64_t mono,const br_schedule_command *cmd){
    if(!r||r->status!=BR_RUN_IDLE||!r->candidate||!run||run<=r->last_run||!epoch||!cmd||
       !cmd->revision||!cmd->task_id||!cmd->open||!cmd->length||cmd->length>8192||mono!=r->now||
       c.script!=r->next.script||c.slot!=r->next.slot)return -1;
    br_schedule *s=r->scripts[c.script];
    if(strcmp(cmd->revision,s->revision)||strcmp(cmd->task_id,s->tasks[c.slot].id))return -1;
    int busy;if(!registry(r,&busy)||busy)return -1;
    br_discovery *d=r->op->discovery;
    if(!d||d->phase!=BR_DISC_READY||d->epoch!=epoch||!d->caps.ready||d->caps.connection!=epoch)return -1;
    int acquired=r->io.acquire(r->io.context,run);
    if(!acquired)return BR_RUN_IDLE;
    if(acquired!=1){
        /* A broken acquire contract may have taken ownership. Request cleanup
         * with the new generation even though no durable claim/send exists. */
        r->run=r->last_run=run;r->epoch=epoch;r->closed=0;return blocked(r);
    }
    int rc=br_schedule_claim(s,c.slot,mono,epoch,uuid);
    /* Even claim failure must release the acquired owner. No business sent. */
    own(r,run,c.script,c.slot,mono);r->epoch=epoch;
    if(rc!=1){r->status=BR_RUN_BLOCKED;close_owned(r);return r->status;}
    if(br_schedule_admit(s,c.slot,r->serial,&d->caps,cmd->open,cmd->length,NULL,&d->scratch)!=BR_CAPS_OK)
        return fail(r,1,0);
    size_t n=0;
    if(br_operation_prepare(r->op,epoch,mono,cmd->open,cmd->length,r->wire,sizeof(r->wire),&n)!=BR_OP_PREPARED){
        memset(r->wire,0,sizeof(r->wire));return fail(r,1,0);
    }
    r->oneway=r->op->plan.oneway;r->timeout=r->op->timeout_ms;r->status=BR_RUN_WAIT;
    rc=r->io.send(r->io.context,run,epoch,r->wire,n);memset(r->wire,0,sizeof(r->wire));
    if(rc!=1)return fail(r,rc==0,0);
    return r->status;
}
int br_schedule_runner_recover(br_schedule_runner *r,uint64_t run,unsigned i,unsigned slot,uint64_t mono){
    if(!r||(r->journal&&r->journal->busy)||r->status!=BR_RUN_IDLE||!run||run<=r->last_run||i>=r->count||slot>=r->scripts[i]->count||
       (r->sampled&&mono<r->now))return -1;
    br_schedule *s=r->scripts[i];br_schedule_record *rec=s->records+slot;
    if(s->blocked||!s->owned||rec->serial!=s->owned||!rec->epoch||
       (!active(rec)&&rec->phase!=BR_SCHED_PENDING&&rec->phase!=BR_SCHED_TERMINAL))return -1;
    /* A second unresolved instance is a conflict, not permission to run. */
    for(unsigned a=0;a<r->count;a++)for(unsigned b=0;b<r->scripts[a]->count;b++)
        if((a!=i||b!=slot)&&(active(r->scripts[a]->records+b)||
           (r->scripts[a]->owned&&r->scripts[a]->records[b].serial==r->scripts[a]->owned)))return -1;
    own(r,run,i,slot,mono);
    if(active(rec)&&br_schedule_failed(s,r->serial,mono,0,0)!=1)return blocked(r);
    r->status=active(rec)?BR_RUN_UNKNOWN:BR_RUN_DRAINING;close_owned(r);return r->status;
}
static int current(br_schedule_runner *r,uint64_t run){
    return r&&(!r->journal||!r->journal->busy)&&!r->flow_active&&run&&run==r->run&&r->status!=BR_RUN_IDLE&&r->status!=BR_RUN_BLOCKED;
}
static int clock_check(br_schedule_runner *r,uint64_t mono){
    if(mono<r->now){(void)fail(r,0,0);return 0;}
    r->now=mono;return 1;
}
static int event(br_schedule_runner *r,int ev){
    if(ev==BR_OP_ACK){
        if(br_schedule_ack(script(r),r->serial,r->epoch,r->command,r->op->task_id)!=1)return blocked(r);
        r->acked=1;
    }else if(ev==BR_OP_READY||ev==BR_OP_ERROR){
        if(r->op->confirmed_audio)return fail(r,0,0);
        r->status=BR_RUN_RECEIPT;
    }else if(ev==BR_OP_CLOSED||ev==BR_OP_TIMEOUT||ev==BR_OP_INVALID){
        if(r->oneway&&r->acked){close_owned(r);return r->status;}
        return fail(r,0,0);
    }
    return r->status;
}
int br_schedule_runner_receive(br_schedule_runner *r,uint64_t run,uint64_t epoch,uint64_t mono,const void *data,size_t n){
    if(!current(r,run)||epoch!=r->epoch)return r?(int)r->status:-1;
    if(r->status!=BR_RUN_WAIT||r->closed)return r->status;
    if(!clock_check(r,mono))return r->status;
    if(mono-r->started>=r->timeout)return expired(r);
    return event(r,br_operation_receive(r->op,epoch,mono,data,n));
}
int br_schedule_runner_finish(br_schedule_runner *r,uint64_t run,int proof){
    if(!current(r,run)||r->status!=BR_RUN_RECEIPT||proof!=1)return -1;
    if(br_schedule_finish(script(r),r->serial,r->epoch,r->command,proof)!=1)return blocked(r);
    r->status=BR_RUN_DRAINING;close_owned(r);return r->status;
}
static br_result *sync_identity(br_schedule_runner *r){
    br_result *v=calloc(1,sizeof(*v));if(!v)return NULL;
    v->origin=1;v->epoch=r->epoch;memcpy(v->command_id,r->command,37);
    br_schedule *s=script(r);
    memcpy(v->local.script,s->script,sizeof(s->script));
    memcpy(v->local.revision,s->revision,sizeof(s->revision));
    memcpy(v->local.task,s->tasks[r->slot].id,sizeof(v->local.task));v->local.serial=r->serial;
    return v;
}
static int sync_done(br_schedule_runner *r,int rc){
    if(rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT||rc==BR_DUPLICATE){
        if(br_schedule_finish(script(r),r->serial,r->epoch,r->command,1)!=1){(void)blocked(r);return BR_IO;}
        r->status=BR_RUN_DRAINING;close_owned(r);
    }
    return rc;
}
int br_schedule_runner_sync(br_schedule_runner *r,uint64_t run,br_inbox *q,uint64_t wall,int trusted){
    return br_schedule_runner_sync_policy(r,run,q,wall,trusted,BR_SPEECH_ORIGINAL);
}
int br_schedule_runner_sync_policy(br_schedule_runner *r,uint64_t run,br_inbox *q,uint64_t wall,int trusted,enum br_speech_policy policy){
    if(!current(r,run)||r->status!=BR_RUN_RECEIPT||!q||!r->op||r->op->epoch!=r->epoch||
       strcmp(r->op->command_id,r->command)||r->op->confirmed_audio||
       (r->op->last_event!=BR_OP_READY&&r->op->last_event!=BR_OP_ERROR))return BR_INVALID;
    br_result *v=sync_identity(r);if(!v)return BR_IO;
    const br_json *d=&r->op->response;int rc=BR_INVALID;
    unsigned n=br_json_field(d,1,r->op->last_event==BR_OP_READY?"text":"msg");
    if(n){
        br_text t=br_json_text(d,n);
        if(br_json_kind(d,n)!=BR_J_STRING||t.length>=sizeof(v->text)||memchr(t.data,0,t.length))goto done;
        memcpy(v->text,t.data,t.length);
    }
    if(r->op->last_event==BR_OP_ERROR){
        if(!r->op->error||strlen(r->op->error)>=sizeof(v->code))goto done;
        strcpy(v->code,r->op->error);
    }
    const br_schedule_task *t=script(r)->tasks+r->slot;
    rc=br_inbox_sync_receive_policy(q,v,wall,trusted,(int)t->audible,t->announce_ttl_s,policy);
done:memset(v,0,sizeof(*v));free(v);return sync_done(r,rc);
}
int br_schedule_runner_resume_receipt(br_schedule_runner *r,uint64_t run,br_inbox *q){
    if(!current(r,run)||r->status!=BR_RUN_UNKNOWN||!q)return BR_INVALID;
    br_result *v=sync_identity(r);if(!v)return BR_IO;
    int rc=br_inbox_sync_receipt(q,v,(int)script(r)->tasks[r->slot].audible);
    memset(v,0,sizeof(*v));free(v);
    if(rc!=1)return rc;
    rc=sync_done(r,BR_DUPLICATE);return rc==BR_DUPLICATE?1:rc;
}
int br_schedule_runner_result(br_schedule_runner *r,uint64_t run,const br_result *v,br_inbox *q,
                             int live,uint64_t connection,uint64_t wall,int trusted){
    return br_schedule_runner_result_policy(r,run,v,q,live,connection,wall,trusted,BR_SPEECH_ORIGINAL);
}
int br_schedule_runner_result_policy(br_schedule_runner *r,uint64_t run,const br_result *v,br_inbox *q,
                                    int live,uint64_t connection,uint64_t wall,int trusted,enum br_speech_policy policy){
    if(!current(r,run)||!v||!q||!br_result_valid(v))return BR_INVALID;
    br_schedule_record *rec=record(r);
    if(!active(rec)||v->epoch!=r->epoch||strcmp(v->command_id,r->command)||
       (*rec->remote&&strcmp(v->task_id,rec->remote)))return BR_INVALID;
    const br_schedule_task *t=script(r)->tasks+r->slot;
    int rc=br_inbox_receive_policy(q,v,live,connection,wall,trusted,(int)t->audible,t->announce_ttl_s,policy);
    if(rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT||rc==BR_DUPLICATE){
        if(br_schedule_finish(script(r),r->serial,r->epoch,r->command,1)!=1){(void)blocked(r);return BR_IO;}
        r->status=BR_RUN_DRAINING;close_owned(r);
    }
    return rc;
}
int br_schedule_runner_tick(br_schedule_runner *r,uint64_t run,uint64_t mono,int preempt){
    if(!current(r,run))return r?(int)r->status:-1;
    if(preempt!=0&&preempt!=1)return -1;
    if(!clock_check(r,mono))return r->status;
    if((r->status==BR_RUN_WAIT||r->status==BR_RUN_RECEIPT)&&preempt)(void)fail(r,0,0);
    if(r->status==BR_RUN_RECEIPT&&mono-r->started>=r->timeout)(void)expired(r);
    if(r->status==BR_RUN_WAIT){
        if(mono-r->started>=r->timeout)(void)expired(r);
        else if(!r->closed)(void)event(r,br_operation_poll(r->op,mono));
    }
    if(r->status==BR_RUN_DRAINING&&r->io.reaped(r->io.context,r->run,r->epoch)==1){
        if(br_schedule_release(script(r),r->serial,1)!=1)return blocked(r);
        r->status=BR_RUN_IDLE;r->run=0;r->serial=r->epoch=0;memset(r->command,0,sizeof(r->command));
    }
    return r->status;
}
int br_schedule_runner_timeout_notice(br_schedule_runner *r,uint64_t run,uint64_t mono,
                                      br_inbox *q,uint64_t wall,int trusted){
    if(!current(r,run)||!q||mono<r->now||(trusted!=0&&trusted!=1))return BR_INVALID;
    if(r->status==BR_RUN_WAIT||r->status==BR_RUN_RECEIPT){
        if(mono-r->started<r->timeout)return BR_INVALID;
    }else if(r->status!=BR_RUN_UNKNOWN||!r->timed_out)return BR_INVALID;
    (void)br_schedule_runner_tick(r,run,mono,0);
    if(r->status!=BR_RUN_UNKNOWN||!r->timed_out)return BR_IO;
    br_result *v=sync_identity(r);if(!v)return BR_IO;
    const br_schedule_task *t=script(r)->tasks+r->slot;
    int rc=br_inbox_timeout(q,v,wall,trusted,(int)t->audible,t->announce_ttl_s);
    memset(v,0,sizeof(*v));free(v);return rc;
}
