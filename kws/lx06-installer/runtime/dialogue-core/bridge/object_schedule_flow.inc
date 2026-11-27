/* Same original compact global set, A saved-source ownership and B30
 * fixed-session coordinator. This file is part of object_schedule_compact.c. */
static int compact_flow_current(br_compact_schedule_session *x){
    return x&&x->prepared&&x->set&&x->set->session==x&&x->source&&x->owner&&
        x->source->owner==x->owner&&eo_same(&x->origin,&x->owner->origin)&&
        eo_saved_current(x->source,&x->origin)==1&&compact_current(x->set);
}
static int compact_flow_binding(br_compact_schedule_session *x,br_flow_log_binding *b,br_flow_session_log_binding *v){
    br_compact_schedule *set=x->set;br_schedule_runner *r=set->runner;
    if(x->selected.script>=set->count||x->selected.slot>=set->scripts[x->selected.script].count||
       !x->object->inputs_verified||!x->object->definition.count||
       !eo_same(&x->object->origin,&x->origin)||!x->run||!x->session_run||!x->epoch||!x->generation)return 0;
    br_schedule *s=set->scripts[x->selected.script].schedule;
    const br_schedule_task *task=s->tasks+x->selected.slot;
    if(r->scripts[x->selected.script]!=s||s->sequence==UINT64_MAX)return 0;
    memset(b,0,sizeof(*b));memset(v,0,sizeof(*v));b->run=x->run;b->serial=s->sequence+1;
    b->count=(unsigned)x->object->definition.count;b->audible=task->audible;b->silent=!b->audible;
    strcpy(b->script,s->script);strcpy(b->revision,s->revision);strcpy(b->task,task->id);
    struct eo_schedule_hash hash;unsigned char digest[32];static const char hex[]="0123456789abcdef";
    int ok=eo_schedule_hash_init(&hash)&&eo_schedule_hash_update(&hash,x->origin.flow,64)&&
        eo_schedule_hash_update(&hash,x->name,strlen(x->name))&&eo_schedule_hash_done(&hash,digest);
    memset(&hash,0,sizeof hash);if(!ok)return 0;
    for(unsigned i=0;i<32;i++){b->definition_sha256[2*i]=hex[digest[i]>>4];b->definition_sha256[2*i+1]=hex[digest[i]&15];}
    memset(digest,0,sizeof digest);strcpy(b->inputs_sha256,x->object->inputs_sha256);
    strcpy(v->script,b->script);strcpy(v->flow_sha256,x->origin.flow);strcpy(v->revision,b->revision);
    strcpy(v->definition_sha256,b->definition_sha256);strcpy(v->inputs_sha256,b->inputs_sha256);
    v->run=x->run;v->session_run=x->session_run;v->epoch=x->epoch;v->generation=x->generation;
    v->count=b->count;v->source=BR_LOG_SCHEDULED;
    return br_flow_log_binding_valid(b)&&br_flow_session_log_binding_valid(v);
}
static int compact_session_prepare(br_compact_schedule_session *x,br_compact_schedule *set,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity,int guarded){
    if(!inputs||capacity<8193||!binding)return -1;
    memset(inputs,0,capacity);memset(binding,0,sizeof(*binding));
    if(!x||x->prepared||x->busy||x->held||x->started||!set||set->busy!=guarded||set->session||set->held||
       !clock||!saved||saved->active||!owner||owner->view||owner->busy||owner->in_flight||!object||
       !adapter||adapter->busy||adapter->bound||adapter->active||!adapter->session||adapter->session->busy||
       !adapter->session->live||adapter->session->owned||!run||!generation||!compact_shape(set)||
       set->runner->op!=&adapter->session->operation||run<=set->runner->last_run||!set->runner->candidate)return -1;
    set->busy=x->busy=1;int rc=-1;br_schedule_candidate prior=set->runner->next,chosen;
    struct eo_origin origin;char revision[65];size_t length=0;
    if(compact_poll(set,clock,&chosen)!=1||chosen.script!=prior.script||chosen.slot!=prior.slot)goto done;
    struct eo_compact_schedule_source *source=set->scripts[chosen.script].source;
    const struct eo_schedule_run_ref *ref=set->runs+set->scripts[chosen.script].first+chosen.slot;
    if(ref->kind!=EO_SCHEDULE_FLOW||!compact_revision(source,&origin,revision)||
       eo_compact_schedule_flow_select(set->work,source,ref,&origin,x->name,sizeof x->name,inputs,8193,&length)!=1||
       eo_compact_schedule_flow_load(set->work,source,&origin,saved,owner,flow_directory,flow_parent,flow_entry)!=1)goto done;
    if(eo_inputs_prepare(saved,x->name,inputs,length)!=1||br_object_flow_copy_prepared(object,owner,&owner->origin,x->name)!=1)goto clear;
    for(size_t i=0;i<object->definition.count;i++)if(object->steps[i].audio)goto clear;
    x->set=set;x->source=saved;x->owner=owner;x->object=object;x->adapter=adapter;x->origin=owner->origin;x->selected=chosen;
    x->run=run;x->session_run=adapter->session->run;x->epoch=adapter->session->epoch;x->generation=generation;x->original_io=adapter->io;
    br_flow_log_binding b;
    if(!compact_flow_binding(x,&b,binding)||!compact_current(set)||eo_saved_current(saved,&x->origin)!=1)goto clear;
    x->prepared=1;set->session=x;rc=1;goto done;
clear:
    if(saved->active)(void)eo_saved_close(saved);(void)eo_revoke(owner);memset(object,0,sizeof(*object));
done:
    if(rc!=1){memset(x,0,sizeof(*x));memset(binding,0,sizeof(*binding));}
    memset(inputs,0,capacity);memset(&origin,0,sizeof origin);memset(revision,0,sizeof revision);set->busy=guarded;x->busy=0;return rc;
}
int br_compact_schedule_session_prepare(br_compact_schedule_session *x,br_compact_schedule *set,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity){
    return compact_session_prepare(x,set,clock,saved,owner,object,adapter,run,generation,flow_directory,flow_parent,flow_entry,binding,inputs,capacity,0);
}
int br_compact_schedule_session_discard(br_compact_schedule_session *x){
    if(!x||x->busy||!x->prepared||x->begun||x->held||x->started||!x->set||x->set->session!=x||x->set->busy)return -1;
    if(eo_saved_close(x->source)||eo_revoke(x->owner))return -1;
    x->set->session=NULL;memset(x->object,0,sizeof(*x->object));memset(x,0,sizeof(*x));return 1;
}
typedef struct {br_compact_schedule_session *x;br_schedule_run_io original;} compact_flow_claim;
static int compact_flow_acquire(void *context,uint64_t run){
    compact_flow_claim *q=context;int rc=q->original.acquire(q->original.context,run);
    return rc==1&&!compact_flow_current(q->x)?-1:rc;
}
static int compact_flow_claim_send(void *context,uint64_t run,uint64_t epoch,const void *bytes,size_t length){
    compact_flow_claim *q=context;return q->original.send(q->original.context,run,epoch,bytes,length);
}
static void compact_flow_claim_close(void *context,uint64_t run,uint64_t epoch,uint64_t now){
    compact_flow_claim *q=context;q->original.close(q->original.context,run,epoch,now);
}
static int compact_flow_claim_reaped(void *context,uint64_t run,uint64_t epoch){
    compact_flow_claim *q=context;return q->original.reaped(q->original.context,run,epoch);
}
static int compact_flow_send(void *context,uint64_t run,uint64_t session,uint64_t ticket,uint64_t epoch,uint64_t generation,const void *bytes,size_t length){
    br_compact_schedule_session *x=context;
    if(!x||!x->busy||!x->held||!x->started||run!=x->run||session!=x->session_run||
       epoch!=x->epoch||generation!=x->generation||!compact_flow_current(x))return 0;
    return x->original_io.send(x->original_io.context,run,session,ticket,epoch,generation,bytes,length);
}
static void compact_flow_event(void *context,uint64_t run,uint64_t ticket,const br_operation *operation,int event){
    br_compact_schedule_session *x=context;x->original_io.event(x->original_io.context,run,ticket,operation,event);
}
static int compact_flow_unsent(void *context,const struct eo_view *view,const struct eo_origin *origin){
    br_compact_schedule_session *x=context;br_schedule_runner *r=x->set->runner;
    return view&&eo_same(origin,&x->origin)&&!x->started&&r->status==BR_RUN_IDLE&&!r->run&&r->last_run<x->run;
}
static int compact_flow_cancel_unclaimed(br_compact_schedule_session *x,uint64_t now){
    br_flow_session *a=x->adapter;br_schedule_runner *r=x->set->runner;br_flow *f=a->flow;
    if(!x->begun)return 1;
    if(x->started||x->held||x->set->held||r->status!=BR_RUN_IDLE||r->run||r->last_run>=x->run||
       a->bound||a->active||f->run!=x->run||f->status!=BR_FLOW_NEXT||f->ticket||f->index||
       f->checkpoint||f->session_epoch)return 0;
    /* The original flow lifecycle cancels this never-dispatched NEXT; no
     * cancel callback, durable claim or transport action is invented here. */
    if(br_flow_poll(f,x->run,now,1,&r->op->discovery->scratch)!=BR_FLOW_CANCELLED||br_flow_clear(f)!=1)return 0;
    x->begun=0;return 1;
}
static int compact_calendar_current(br_compact_schedule_session *old){
    if(!old||!old->released||old->held||old->successor_run||!old->calendar_advanced||
       (old->coordinator.deferred?(old->coordinator.retired||old->coordinator.receipt):(!old->coordinator.retired||!old->coordinator.receipt))||
       !old->set||old->selected.script>=old->set->count)return 0;
    br_schedule *s=old->set->scripts[old->selected.script].schedule;
    if(!s||old->selected.slot>=s->count||s->owned||s->sequence!=old->retired_sequence||
       strcmp(s->script,old->coordinator.binding.script)||strcmp(s->revision,old->coordinator.binding.revision)||
       strcmp(s->tasks[old->selected.slot].id,old->coordinator.binding.task))return 0;
    const br_schedule_record *r=s->records+old->selected.slot;
    return ((old->coordinator.deferred&&r->phase==BR_SCHED_PENDING&&r->day==old->retired_day&&
             r->serial==old->coordinator.binding.serial&&r->epoch==old->epoch&&!strcmp(r->command,old->coordinator.command))||
        (r->highwater>=old->retired_day&&((r->phase==BR_SCHED_PENDING&&r->day>old->retired_day)||r->phase==BR_SCHED_WAITING||
         (r->phase==BR_SCHED_TERMINAL&&r->day==old->retired_day&&r->serial==old->coordinator.binding.serial))))&&
        br_schedule_state_current(s)==1;
}
static int compact_predecessor_prefix(br_compact_schedule_session *old,br_inbox *inbox,br_result *scratch,
    br_flow_session_log_closed closed,void *context,int state){
    return old->coordinator.deferred?br_schedule_session_deferred_inspect(&old->coordinator,closed,context,state):
        (old->coordinator.failed?br_schedule_session_failure_retired_inspect:br_schedule_session_retired_inspect)(&old->coordinator,inbox,scratch,closed,context,state);
}
static int compact_session_start(br_compact_schedule_session *x,const struct eo_clock *clock,br_flow_session_log *log,
    br_flow_owner_store *owner,const char *command,br_flow_owner_handoff_current previous,void *context,
    br_compact_schedule_session *old,br_inbox *inbox,br_result *scratch,br_flow_session_log_closed closed,void *closed_context){
    if(!x||x->busy||!x->prepared||x->held||x->started||!x->set||x->set->busy||x->set->session!=x||
       x->predecessor!=old||!clock||!log||log->phase!=(old?(old->coordinator.failed?BR_FLOW_SESSION_LOG_FAILURE_RETIRED:old->coordinator.deferred?BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT:BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED):0)||
       log->readonly||log->blocked||!owner||!command||!previous||
       (old&&(old==x||old->busy||old->set!=x->set||old->coordinator.log!=log||old->coordinator.owner!=owner||
           (!old->coordinator.deferred&&(!inbox||!scratch))||!closed||!compact_calendar_current(old))))return -1;
    br_compact_schedule *set=x->set;br_schedule_runner *r=set->runner;br_flow_session *a=x->adapter;
    if(a->busy||a->bound||a->active||a->io.context!=x->original_io.context||a->io.send!=x->original_io.send||
       a->io.event!=x->original_io.event||!a->session->live||a->session->owned||!r->candidate)return -1;
    x->busy=set->busy=1;if(old)old->busy=1;int rc=-1;br_schedule_candidate chosen;struct eo_origin schedule_origin;
    br_flow_log_binding b;br_flow_session_log_binding expected;
    if(!compact_flow_current(x)||compact_poll(set,clock,&chosen)!=1||chosen.script!=x->selected.script||chosen.slot!=x->selected.slot||
       br_object_flow_copy_prepared(x->object,x->owner,&x->origin,x->name)!=1||!compact_flow_binding(x,&b,&expected)||
       (!old&&memcmp(&expected,&log->binding,sizeof(expected)))||
       (old&&(expected.run<=log->binding.run||expected.session_run<=log->binding.session_run||
           expected.epoch<=log->binding.epoch||expected.generation<=log->binding.generation||
           !strcmp(command,old->coordinator.command)||!compact_calendar_current(old)||
           compact_predecessor_prefix(old,inbox,scratch,closed,closed_context,0)!=1))||
       eo_compact_schedule_origin(set->scripts[chosen.script].source,&schedule_origin)!=1)goto done;
    if(br_flow_begin(a->flow,x->run,clock->mono_ms,&x->object->definition,x->object->inputs,x->object->input_count,
       r->scripts[chosen.script]->script,"scheduled",NULL,&r->op->discovery->scratch)!=1)goto done;
    x->begun=1;
    if((b.silent&&br_flow_require_silent(a->flow,x->run,NULL,0)!=1)||
       !compact_flow_current(x)||eo_hold(x->owner,&x->origin))goto done;
    x->held=1;
    if(eo_compact_schedule_hold(set->work,set->scripts[chosen.script].source,&schedule_origin)!=1){
        if(!eo_release(x->owner,&x->origin,compact_flow_unsent,x))x->held=0;goto done;
    }
    set->held=1;set->held_script=chosen.script;set->held_slot=chosen.slot;
    compact_flow_claim claim={x,r->io};r->io=(br_schedule_run_io){&claim,compact_flow_acquire,compact_flow_claim_send,compact_flow_claim_close,compact_flow_claim_reaped};
    rc=br_schedule_runner_claim_session(r,a->flow,x->run,chosen,x->epoch,command,clock->mono_ms);r->io=claim.original;
    if(r->run==x->run){x->started=1;if(old)old->successor_run=x->run;set->serial=r->serial;set->epoch=r->epoch;memcpy(set->command,r->command,37);}
    if(rc!=BR_RUN_WAIT){
        if(!x->started&&r->status==BR_RUN_IDLE&&!r->run&&r->last_run<x->run){
            if(eo_compact_schedule_release(set->scripts[chosen.script].source,&schedule_origin,compact_no_claim,NULL)==1)set->held=0;
            if(!eo_release(x->owner,&x->origin,compact_flow_unsent,x))x->held=0;
        }goto done;
    }
    int attached=old?(old->coordinator.deferred?
        br_schedule_session_retry(&x->coordinator,&old->coordinator,r,a,log,owner,&b,&expected,previous,context,closed,closed_context):
        (old->coordinator.failed?br_schedule_session_after_failure:br_schedule_session_turnover)(&x->coordinator,&old->coordinator,r,a,log,owner,&b,&expected,inbox,scratch,previous,context,closed,closed_context)):
        br_schedule_session_attach(&x->coordinator,r,a,log,owner,&b,previous,context);
    if(attached!=1){rc=BR_OBJECT_UNVERIFIED;goto done;}
    a->io=(br_flow_session_io){x,compact_flow_send,compact_flow_event};rc=BR_RUN_WAIT;
done:
    if(!x->started&&!x->held&&!set->held&&!compact_flow_cancel_unclaimed(x,clock->mono_ms))rc=BR_OBJECT_UNVERIFIED;
    memset(&schedule_origin,0,sizeof schedule_origin);if(old)old->busy=0;set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_start(br_compact_schedule_session *x,const struct eo_clock *clock,br_flow_session_log *log,
    br_flow_owner_store *owner,const char *command,br_flow_owner_handoff_current previous,void *context){
    return compact_session_start(x,clock,log,owner,command,previous,context,NULL,NULL,NULL,NULL,NULL);
}
static int compact_prepare_successor(br_compact_schedule_session *x,br_compact_schedule_session *old,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity,
    br_inbox *inbox,br_result *scratch,br_flow_session_log_closed closed,void *context,int kind){
    if(!x||x==old||!old||old->busy||!old->released||old->held||old->successor_run||!old->set||old->set->busy||
       old->set->session||old->set->held||!clock||(kind!=1&&(!inbox||!scratch))||!closed||
       old->coordinator.deferred!=(kind==1)||old->coordinator.failed!=(kind==2)||
       !compact_current(old->set)||old->selected.script>=old->set->count)return -1;
    br_compact_schedule *set=old->set;br_schedule *s=set->scripts[old->selected.script].schedule;
    if(!s||old->selected.slot>=s->count||run<=old->run||!adapter||!adapter->session||
       adapter->session->run<=old->session_run||adapter->session->epoch<=old->epoch||generation<=old->generation)return -1;
    set->busy=old->busy=1;int rc=-1;
    if(old->calendar_advanced){if(!compact_calendar_current(old))goto done;}
    if(compact_predecessor_prefix(old,inbox,scratch,closed,context,!old->calendar_advanced)!=1)goto done;
    if(!old->calendar_advanced){old->retired_day=s->records[old->selected.slot].day;old->retired_sequence=s->sequence;old->calendar_advanced=1;}
    br_schedule_candidate chosen;if(compact_poll(set,clock,&chosen)!=1||!compact_calendar_current(old))goto done;
    rc=compact_session_prepare(x,set,clock,saved,owner,object,adapter,run,generation,flow_directory,flow_parent,flow_entry,binding,inputs,capacity,1);
    if(rc==1)x->predecessor=old;
done:if(scratch)memset(scratch,0,sizeof(*scratch));set->busy=old->busy=0;return rc;
}
int br_compact_schedule_session_prepare_next(br_compact_schedule_session *x,br_compact_schedule_session *old,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity,
    br_inbox *inbox,br_result *scratch,br_flow_session_log_closed closed,void *context){
    return compact_prepare_successor(x,old,clock,saved,owner,object,adapter,run,generation,flow_directory,flow_parent,flow_entry,binding,inputs,capacity,inbox,scratch,closed,context,0);
}
int br_compact_schedule_session_prepare_retry(br_compact_schedule_session *x,br_compact_schedule_session *old,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity,
    br_flow_session_log_closed closed,void *context){
    return compact_prepare_successor(x,old,clock,saved,owner,object,adapter,run,generation,flow_directory,flow_parent,flow_entry,binding,inputs,capacity,NULL,NULL,closed,context,1);
}
int br_compact_schedule_session_prepare_after_failure(br_compact_schedule_session *x,br_compact_schedule_session *old,const struct eo_clock *clock,
    struct eo_saved_source *saved,struct eo_owner *owner,br_object_flow *object,br_flow_session *adapter,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *binding,char *inputs,size_t capacity,
    br_inbox *inbox,br_result *scratch,br_flow_session_log_closed closed,void *context){
    return compact_prepare_successor(x,old,clock,saved,owner,object,adapter,run,generation,flow_directory,flow_parent,flow_entry,binding,inputs,capacity,inbox,scratch,closed,context,2);
}
int br_compact_schedule_session_start_next(br_compact_schedule_session *x,const struct eo_clock *clock,const char *command,
    br_inbox *inbox,br_result *scratch,br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_closed closed,void *closed_context){
    if(!x||!x->predecessor||x->predecessor->coordinator.deferred||x->predecessor->coordinator.failed)return -1;br_compact_schedule_session *old=x->predecessor;
    return compact_session_start(x,clock,old->coordinator.log,old->coordinator.owner,command,previous,previous_context,
        old,inbox,scratch,closed,closed_context);
}
int br_compact_schedule_session_start_retry(br_compact_schedule_session *x,const struct eo_clock *clock,const char *command,
    br_flow_owner_handoff_current previous,void *previous_context,br_flow_session_log_closed closed,void *closed_context){
    if(!x||!x->predecessor||!x->predecessor->coordinator.deferred)return -1;br_compact_schedule_session *old=x->predecessor;
    return compact_session_start(x,clock,old->coordinator.log,old->coordinator.owner,command,previous,previous_context,
        old,NULL,NULL,closed,closed_context);
}
int br_compact_schedule_session_start_after_failure(br_compact_schedule_session *x,const struct eo_clock *clock,const char *command,
    br_inbox *inbox,br_result *scratch,br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_closed closed,void *closed_context){
    if(!x||!x->predecessor||!x->predecessor->coordinator.failed||x->predecessor->coordinator.deferred)return -1;
    br_compact_schedule_session *old=x->predecessor;
    return compact_session_start(x,clock,old->coordinator.log,old->coordinator.owner,command,previous,previous_context,
        old,inbox,scratch,closed,closed_context);
}
static int compact_flow_active(br_compact_schedule_session *x){
    return x&&!x->busy&&x->prepared&&x->held&&x->started&&!x->released&&x->set&&!x->set->busy&&
        x->set->session==x&&x->coordinator.attached&&x->owner->in_flight&&eo_same(&x->origin,&x->owner->origin);
}
int br_compact_schedule_session_dispatch(br_compact_schedule_session *x,uint64_t ticket,uint64_t now,const char *command){
    if(!compact_flow_active(x)||!command)return -1;x->busy=x->set->busy=1;int rc;
    if(!compact_flow_current(x)){(void)br_schedule_session_poll(&x->coordinator,x->run,now,1);rc=BR_OBJECT_UNVERIFIED;}
    else rc=br_schedule_session_dispatch(&x->coordinator,x->run,ticket,now,command);
    x->set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_event(br_compact_schedule_session *x,uint64_t now,int event){
    if(!compact_flow_active(x))return -1;x->busy=x->set->busy=1;
    int rc=br_schedule_session_event(&x->coordinator,x->run,now,event);x->set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_poll(br_compact_schedule_session *x,uint64_t now,int preempt){
    if(!compact_flow_active(x))return -1;x->busy=x->set->busy=1;
    int rc=br_schedule_session_poll(&x->coordinator,x->run,now,preempt);x->set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_sync(br_compact_schedule_session *x,br_inbox *inbox,uint64_t wall,int trusted,br_result *scratch){
    if(!compact_flow_active(x))return BR_INVALID;x->busy=x->set->busy=1;
    int rc=br_schedule_session_sync(&x->coordinator,x->run,inbox,wall,trusted,scratch);x->set->busy=x->busy=0;return rc;
}
static void compact_flow_aggregate(br_compact_schedule_session *x,br_result *v){
    memset(v,0,sizeof(*v));v->origin=1;v->epoch=x->epoch;strcpy(v->command_id,x->coordinator.command);
    strcpy(v->local.script,x->coordinator.binding.script);strcpy(v->local.revision,x->coordinator.binding.revision);
    strcpy(v->local.task,x->coordinator.binding.task);v->local.serial=x->coordinator.binding.serial;
}
static int compact_flow_released(void *context,const struct eo_view *view,const struct eo_origin *origin){
    br_compact_schedule_session *x=context;br_schedule_runner *r=x->set->runner;br_schedule_session *c=&x->coordinator;
    br_schedule *s=x->set->scripts[x->selected.script].schedule;const br_schedule_record *record=s->records+x->selected.slot;
    return view&&eo_same(origin,&x->origin)&&c->retired&&c->receipt&&!c->log->blocked&&
        c->log->phase==BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED&&c->log->binding.run==x->run&&
        c->log->index+1==c->binding.count&&r->status==BR_RUN_IDLE&&!s->owned&&
        record->phase==BR_SCHED_TERMINAL&&record->serial==c->binding.serial&&record->epoch==x->epoch&&
        !strcmp(record->command,c->command)&&r->io.reaped(r->io.context,x->run,x->epoch)==1;
}
int br_compact_schedule_session_retire(br_compact_schedule_session *x,br_inbox *inbox,br_result *scratch){
    if(!compact_flow_active(x)||!inbox||!scratch)return -1;x->busy=1;int rc=-1;
    if(!x->coordinator.retired&&br_schedule_session_retire(&x->coordinator,x->run,inbox,scratch)!=1)goto done;
    compact_flow_aggregate(x,scratch);
    int receipt=br_inbox_sync_receipt(inbox,scratch,(int)x->coordinator.binding.audible);memset(scratch,0,sizeof(*scratch));
    if(receipt!=1)goto done;
    if(x->set->held&&compact_release(x->set,inbox,scratch)!=1)goto done;
    if(eo_release(x->owner,&x->origin,compact_flow_released,x))goto done;
    x->held=0;x->released=1;x->set->session=NULL;x->adapter->io=x->original_io;
    (void)eo_inputs_discard(x->owner);rc=1;
done:memset(scratch,0,sizeof(*scratch));x->busy=0;return rc;
}
static int compact_flow_deferred(br_compact_schedule_session *x){
    br_schedule_session *c=&x->coordinator;br_schedule_runner *r=x->set->runner;
    br_schedule *s=x->set->scripts[x->selected.script].schedule;const br_schedule_record *rec=s->records+x->selected.slot;
    return c->deferred&&!c->receipt&&!c->retired&&!c->log->blocked&&!c->owner->blocked&&!s->blocked&&
        c->log->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT&&c->log->binding.run==x->run&&
        !c->log->index&&r->status==BR_RUN_IDLE&&!r->run&&!r->flow_active&&!s->owned&&
        rec->phase==BR_SCHED_PENDING&&rec->serial==c->binding.serial&&rec->epoch==x->epoch&&
        !strcmp(rec->command,c->command)&&r->io.reaped(r->io.context,x->run,x->epoch)==1;
}
static int compact_flow_deferred_source(void *p,const struct eo_origin *origin){
    br_compact_schedule_session *x=p;br_compact_schedule *set=x->set;struct eo_origin expected;
    return eo_compact_schedule_origin(set->scripts[x->selected.script].source,&expected)==1&&
        eo_same(&expected,origin)&&compact_flow_deferred(x);
}
static int compact_flow_deferred_owner(void *p,const struct eo_view *view,const struct eo_origin *origin){
    br_compact_schedule_session *x=p;return view&&eo_same(origin,&x->origin)&&compact_flow_deferred(x);
}
int br_compact_schedule_session_defer_unsent(br_compact_schedule_session *x,uint64_t now,int auth){
    if(!compact_flow_active(x)||(auth!=0&&auth!=1))return -1;
    x->busy=x->set->busy=1;int rc=-1;br_compact_schedule *set=x->set;
    if(!x->coordinator.deferred){
        rc=br_schedule_session_defer_unsent(&x->coordinator,x->run,now,auth);if(rc!=1)goto done;
    }
    rc=-1;
    if(!compact_flow_deferred(x))goto done;
    if(set->held){
        struct eo_origin origin;
        if(eo_compact_schedule_origin(set->scripts[x->selected.script].source,&origin)!=1||
           eo_compact_schedule_release(set->scripts[x->selected.script].source,&origin,compact_flow_deferred_source,x)!=1)goto done;
        set->held=0;
    }
    if(eo_release(x->owner,&x->origin,compact_flow_deferred_owner,x))goto done;
    x->held=0;x->released=1;set->session=NULL;x->adapter->io=x->original_io;
    (void)eo_inputs_discard(x->owner);rc=1;
done:set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_failure(br_compact_schedule_session *x,uint64_t now,br_inbox *q,uint64_t wall,int trusted,br_result *scratch){
    if(!compact_flow_active(x)||!q||!scratch)return -1;x->busy=x->set->busy=1;
    int rc=br_schedule_session_failure(&x->coordinator,x->run,now,q,wall,trusted,scratch);
    x->set->busy=x->busy=0;return rc;
}
int br_compact_schedule_session_failure_poll(br_compact_schedule_session *x,uint64_t now){
    if(!compact_flow_active(x))return -1;x->busy=x->set->busy=1;
    int rc=br_schedule_session_failure_poll(&x->coordinator,x->run,now);x->set->busy=x->busy=0;return rc;
}
static int compact_failure_released(br_compact_schedule_session *x){
    br_schedule_session *c=&x->coordinator;br_schedule_runner *r=x->set->runner;
    br_schedule *s=x->set->scripts[x->selected.script].schedule;const br_schedule_record *rec=s->records+x->selected.slot;
    return c->failed&&c->retired&&c->receipt&&!c->log->blocked&&!c->owner->blocked&&!s->blocked&&
        c->log->phase==BR_FLOW_SESSION_LOG_FAILURE_RETIRED&&c->log->binding.run==x->run&&
        r->status==BR_RUN_IDLE&&!r->run&&!r->flow_active&&!s->owned&&rec->phase==BR_SCHED_TERMINAL&&
        rec->serial==c->binding.serial&&rec->epoch==x->epoch&&!strcmp(rec->command,c->command)&&
        r->io.reaped(r->io.context,x->run,x->epoch)==1;
}
static int compact_failure_source(void *p,const struct eo_origin *origin){
    br_compact_schedule_session *x=p;struct eo_origin expected;
    return eo_compact_schedule_origin(x->set->scripts[x->selected.script].source,&expected)==1&&
        eo_same(&expected,origin)&&compact_failure_released(x);
}
static int compact_failure_owner(void *p,const struct eo_view *view,const struct eo_origin *origin){
    br_compact_schedule_session *x=p;return view&&eo_same(origin,&x->origin)&&compact_failure_released(x);
}
int br_compact_schedule_session_retire_failure(br_compact_schedule_session *x,br_inbox *q,br_result *scratch){
    if(!compact_flow_active(x)||!q||!scratch||!x->coordinator.failed)return -1;
    x->busy=x->set->busy=1;int rc=-1;br_compact_schedule *set=x->set;const br_result *result=NULL;
    if(!x->coordinator.retired&&br_schedule_session_retire_failure(&x->coordinator,x->run,q,scratch)!=1)goto done;
    compact_flow_aggregate(x,scratch);
    int receipt=br_inbox_sync_result(q,scratch,(int)x->coordinator.binding.audible,&result);
    memset(scratch,0,sizeof(*scratch));
    if(receipt!=1||!result||strcmp(result->code,x->coordinator.log->failure_code)||!compact_failure_released(x))goto done;
    if(set->held){
        struct eo_origin origin;
        if(eo_compact_schedule_origin(set->scripts[x->selected.script].source,&origin)!=1||
           eo_compact_schedule_release(set->scripts[x->selected.script].source,&origin,compact_failure_source,x)!=1)goto done;
        set->held=0;
    }
    if(eo_release(x->owner,&x->origin,compact_failure_owner,x))goto done;
    x->held=0;x->released=1;set->session=NULL;x->adapter->io=x->original_io;
    (void)eo_inputs_discard(x->owner);rc=1;
done:memset(scratch,0,sizeof(*scratch));set->busy=x->busy=0;return rc;
}

int br_compact_schedule_session_associate_task(br_compact_schedule_session *x,br_result *scratch){
    if(!compact_flow_active(x)||!scratch)return BR_INVALID;
    x->busy=x->set->busy=1;
    int rc=compact_flow_current(x)?br_schedule_session_associate_task(&x->coordinator,x->run,scratch):BR_INVALID;
    x->set->busy=x->busy=0;return rc;
}
