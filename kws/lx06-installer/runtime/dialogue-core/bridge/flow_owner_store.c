#include "flow_owner_store.h"
#include <string.h>
static int healthy(const br_flow_owner_store *s){
    return s&&s->store&&s->workspace&&s->current&&s->store->schedule==3&&
        s->store->directory>=0&&s->store->lock>=0;
}
static int log_busy(const br_flow_session_log *j){
    return j&&(j->busy||(j->adapter&&(j->adapter->busy||
        (j->adapter->session&&j->adapter->session->busy))));
}
int br_flow_owner_store_init(br_flow_owner_store *s,br_store *store,void *workspace,
    size_t capacity,br_flow_owner_handoff_current current,void *context){
    if(!s||!store||store->schedule!=3||store->directory<0||store->lock<0||store->poisoned||
       !workspace||capacity!=BR_FLOW_OWNER_HANDOFF_BYTES||!current)return BR_INVALID;
    *s=(br_flow_owner_store){.store=store,.workspace=workspace,.current=current,.context=context};return 1;
}
static int acquire(br_flow_owner_store *s){
    if(!healthy(s)||s->busy)return BR_INVALID;
    if(s->blocked||s->store->poisoned)return BR_IO;
    s->busy=1;return 1;
}
static int finish(br_flow_owner_store *s,int rc){
    memset(s->workspace,0,BR_FLOW_OWNER_HANDOFF_BYTES);s->busy=0;return rc;
}
static int load(br_flow_owner_store *s,size_t *n){
    int rc=br_store_read(s->store,s->workspace,BR_FLOW_OWNER_HANDOFF_BYTES,n);
    if(rc<0||(rc==1&&br_flow_owner_handoff_validate(s->workspace,*n)!=1)){
        s->blocked=1;return BR_IO;
    }
    return rc;
}
static int admission_proven(br_flow_session_log *live,int proof){
    int current=!live||br_flow_session_log_live_current(live)==1;
    return current&&proof==1;
}
static int commit_checked(br_flow_owner_store *s,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count,
    br_flow_session_log *live,br_flow_session *target){
    size_t n=0;
    if(br_flow_owner_handoff_encode(b,steps,ids,count,s->workspace,BR_FLOW_OWNER_HANDOFF_BYTES,&n)!=1)return BR_INVALID;
    int proof=s->current(s->context,s->workspace,n);
    int current=target?br_flow_session_log_target_current(live,target,b)==1:
        admission_proven(live,1);
    if(!current||proof!=1)return BR_INVALID;
    if(br_store_write(s->store,s->workspace,n)){s->blocked=1;return BR_IO;}
    return 1;
}
static int commit(br_flow_owner_store *s,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count){
    return commit_checked(s,b,steps,ids,count,NULL,NULL);
}
static int publish(br_flow_owner_store *s,unsigned event,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count,
    br_flow_owner_handoff_current previous,void *context,br_flow_session_log *live){
    /* Bound handles must use the original two-file coordinator. Even an idle
     * legacy write could replace the image behind its installed checkpoint. */
    if(s&&s->log)return BR_INVALID;
    int rc=acquire(s);if(rc!=1)return rc;
    size_t n=0;
    if(!previous)return finish(s,BR_INVALID);
    rc=load(s,&n);if(rc<0)return finish(s,rc);
    if(!admission_proven(live,previous(context,rc?s->workspace:NULL,n)))return finish(s,BR_INVALID);
    if(event&&br_flow_owner_handoff_transition(rc?s->workspace:NULL,n,event,b,steps,ids,count)!=1)
        return finish(s,BR_INVALID);
    return finish(s,commit_checked(s,b,steps,ids,count,live,NULL));
}
int br_flow_owner_store_save(br_flow_owner_store *s,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count,
    br_flow_owner_handoff_current previous,void *context){
    return publish(s,0,b,steps,ids,count,previous,context,NULL);
}
int br_flow_owner_store_publish(br_flow_owner_store *s,unsigned event,
    const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,
    const br_result *const *ids,unsigned count,br_flow_owner_handoff_current previous,void *context){
    /* Zero is reserved solely for the pre-existing explicit whole-save API. */
    if(event<BR_OWNER_INIT||event>BR_OWNER_ASSOCIATE)return BR_INVALID;
    return publish(s,event,b,steps,ids,count,previous,context,NULL);
}
static int prepared(void *p,const br_flow *f,unsigned event){
    br_flow_owner_store *s=p;int rc=acquire(s);if(rc!=1)return -1;
    br_flow_session_log *j=s->log;size_t n=0;
    if(!j||!j->busy||j->readonly||j->blocked||!j->adapter||!j->adapter->busy||
       !j->adapter->session||!j->adapter->session->busy||j->adapter->flow!=f||
       event!=BR_FLOW_PREPARE||j->phase!=BR_FLOW_PREPARE||j->index>=j->binding.count||
       j->steps[j->index].ticket!=f->ticket||strcmp(j->steps[j->index].command,f->command)||
       !s->previous){rc=BR_INVALID;goto done;}
    rc=load(s,&n);if(rc<0)goto done;
    if(rc!=1||!admission_proven(j,s->previous(s->previous_context,s->workspace,n))||
       br_flow_owner_handoff_append_prepare(s->workspace,BR_FLOW_OWNER_HANDOFF_BYTES,&n,
           &j->binding,j->steps,j->index+1)!=1||!admission_proven(j,s->current(s->context,s->workspace,n))){
        rc=BR_INVALID;goto done;
    }
    rc=br_store_write(s->store,s->workspace,n)?BR_IO:1;
done:
    if(rc!=1)s->blocked=1;
    return finish(s,rc)==1?1:-1;
}
static int pair_available(const br_flow_owner_store *s,const br_flow_session_log *j){
    /* Validate the requested pair AND original IO before taking its wire loan.
     * A foreign/readonly target cannot hide the original producer's guards. */
    return s&&j&&!log_busy(j)&&!log_busy(s->log)&&
        (j->readonly?(!j->adapter&&!s->log):
         (s->log==j&&j->prepared==prepared&&j->prepared_context==s&&
          j->adapter&&j->adapter->wire==s->workspace));
}
int br_flow_owner_store_bind(br_flow_owner_store *s,br_flow_session_log *j,
    br_flow_owner_handoff_current previous,void *context){
    if(!healthy(s)||s->busy||s->log||!previous||!j||j->busy||j->blocked||j->readonly||
       j->prepared||j->phase!=BR_FLOW_SESSION_LOG_BOUND||!j->adapter||
       !j->adapter->flow||!j->adapter->session||log_busy(j)||j->adapter->active||
       s->workspace!=j->adapter->wire||
       j->adapter->flow->status!=BR_FLOW_NEXT||j->adapter->flow->ticket)return BR_INVALID;
    j->busy=j->adapter->busy=j->adapter->session->busy=1;
    int rc=publish(s,BR_OWNER_INIT,&j->binding,NULL,NULL,0,previous,context,j);
    j->adapter->session->busy=j->adapter->busy=j->busy=0;
    if(rc!=1){j->blocked=1;return rc;}
    rc=acquire(s);if(rc!=1){j->blocked=1;return rc;}
    s->log=j;s->previous=previous;s->previous_context=context;
    rc=br_flow_session_log_set_prepare(j,prepared,s)==1?1:BR_INVALID;
    if(rc!=1){s->blocked=1;j->blocked=1;}
    return finish(s,rc);
}
typedef struct {
    br_flow_owner_store *store;const br_result *const *ids;unsigned count;
    br_flow_owner_handoff_current previous;void *context;int rc;
} association_attempt;
static int associate_snapshot(void *p,const br_flow_session_log *j){
    association_attempt *v=p;br_flow_owner_store *s=v->store;
    size_t n=0;unsigned published=0;
    unsigned count=j->phase==BR_FLOW_SESSION_LOG_BOUND?0:j->index+1;
    if(v->count!=count)return -1;
    int rc=load(s,&n);
    if(rc<0){v->rc=rc;return -1;}
    if(rc!=1||v->previous(v->context,s->workspace,n)!=1||
       br_flow_owner_handoff_prefix(s->workspace,n,&j->binding,j->steps,count,&published)!=1||
       published!=count||br_flow_owner_handoff_transition(s->workspace,n,BR_OWNER_ASSOCIATE,
           &j->binding,j->steps,v->ids,count)!=1)return -1;
    v->rc=commit(s,&j->binding,j->steps,v->ids,count);
    return v->rc==1?1:-1;
}
int br_flow_owner_store_associate(br_flow_owner_store *s,br_flow_session_log *j,
    const br_result *const *ids,unsigned count,br_flow_owner_handoff_current previous,void *context){
    /* Refuse before taking/erasing a wire loan already held by original IO. */
    if(!pair_available(s,j)||!ids||!count||count>BR_FLOW_STEPS||!previous)return BR_INVALID;
    int rc=acquire(s);if(rc!=1)return rc;
    association_attempt attempt={s,ids,count,previous,context,BR_INVALID};
    rc=br_flow_session_log_inspect(j,associate_snapshot,&attempt);
    return finish(s,rc==1?1:(j->blocked||(j->store&&j->store->poisoned)?BR_IO:attempt.rc));
}
typedef struct {
    br_flow_owner_store *store;br_flow_session_log_closed closed;void *context;int rc;
} retirement_attempt;
static int retirement_image(void *p,const br_flow_session_log *j){
    retirement_attempt *v=p;br_flow_owner_store *s=v->store;size_t n=0;unsigned count=0;
    int rc=load(s,&n);if(rc<0){v->rc=rc;return -1;}
    return rc==1&&br_flow_owner_handoff_prefix(s->workspace,n,&j->binding,j->steps,
        j->binding.count,&count)==1&&count==j->binding.count&&
        s->current(s->context,s->workspace,n)==1?1:-1;
}
static int retirement_closed(void *p,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,unsigned count){
    retirement_attempt *v=p;return v->closed(v->context,b,steps,count);
}
static int retire_pair(br_flow_owner_store *s,br_flow_session_log *j,
    br_flow_session_log_closed closed,void *context,int live){
    if(!pair_available(s,j)||!closed||(live&&j->readonly))return BR_INVALID;
    int rc=acquire(s);if(rc!=1)return rc;
    retirement_attempt attempt={s,closed,context,BR_INVALID};
    rc=live?br_flow_session_log_retire_live_inspected(j,retirement_image,retirement_closed,&attempt):
        br_flow_session_log_retire_inspected(j,retirement_image,retirement_closed,&attempt);
    return finish(s,rc==1?1:(j->blocked||(j->store&&j->store->poisoned)?BR_IO:attempt.rc));
}
int br_flow_owner_store_retire_closed(br_flow_owner_store *s,br_flow_session_log *j,
    br_flow_session_log_closed closed,void *context){return retire_pair(s,j,closed,context,0);}
int br_flow_owner_store_retire_live(br_flow_owner_store *s,br_flow_session_log *j,
    br_flow_session_log_handed_off handed_off,void *context){return retire_pair(s,j,handed_off,context,1);}
typedef struct {
    retirement_attempt retirement;
    br_flow_owner_handoff_current previous;void *context;
    int live;
    br_flow_session_log *log;br_flow_session *target;const br_flow_session_log_binding *binding;
} replacement_attempt;
static int replacement_before(void *p,const br_flow_session_log *j){
    replacement_attempt *v=p;br_flow_owner_store *s=v->retirement.store;
    size_t n=0;unsigned count=0;int rc=load(s,&n);
    if(rc<0){v->retirement.rc=rc;return -1;}
    return rc==1&&v->previous(v->context,s->workspace,n)==1&&
        (v->live?br_flow_session_log_live_current(s->log)==1:
            br_flow_session_log_target_current(v->log,v->target,v->binding)==1)&&
        br_flow_owner_handoff_prefix(s->workspace,n,&j->binding,j->steps,
            j->binding.count,&count)==1&&count==j->binding.count&&
        s->current(s->context,s->workspace,n)==1?1:-1;
}
static int replacement_closed(void *p,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,unsigned count){
    replacement_attempt *v=p;return retirement_closed(&v->retirement,b,steps,count);
}
static int replacement_after(void *p,const br_flow_session_log *j){
    replacement_attempt *v=p;
    v->retirement.rc=commit_checked(v->retirement.store,&j->binding,NULL,NULL,0,
        v->log,v->live?NULL:v->target);
    return v->retirement.rc==1?1:-1;
}
static int replace_pair(br_flow_owner_store *s,br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,
    br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_closed closed,void *closed_context,int live){
    /* Never erase an outer wire loan, including a fresh adapter's active IO. */
    if(!pair_available(s,j)||!b||!a||a->wire!=s->workspace||!previous||!closed||
       (live&&j->readonly)||a->busy||(a->session&&a->session->busy))return BR_INVALID;
    int rc=acquire(s);if(rc!=1)return rc;
    replacement_attempt attempt={{s,closed,closed_context,BR_INVALID},previous,previous_context,live,j,a,b};
    if(live){
        br_flow_session_log_turnover_ops ops={replacement_before,replacement_after,
            replacement_closed,prepared,&attempt,s};
        rc=br_flow_session_log_rebind_live_inspected(j,b,a,&ops);
    }else{
        br_flow_session_log_rebind_ops ops={replacement_before,replacement_after,
            replacement_closed,prepared,&attempt,s};
        rc=br_flow_session_log_rebind_inspected(j,b,a,&ops);
    }
    if(rc==1){s->log=j;s->previous=previous;s->previous_context=previous_context;}
    return finish(s,rc==1?1:(j->blocked||(j->store&&j->store->poisoned)?BR_IO:attempt.retirement.rc));
}
int br_flow_owner_store_rebind_closed(br_flow_owner_store *s,br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,
    br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_closed closed,void *closed_context){
    return replace_pair(s,j,b,a,previous,previous_context,closed,closed_context,0);
}
int br_flow_owner_store_rebind_live(br_flow_owner_store *s,br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,
    br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_handed_off handed_off,void *handoff_context){
    return replace_pair(s,j,b,a,previous,previous_context,handed_off,handoff_context,1);
}
static int query_available(const br_flow_owner_store *s,const br_flow_session_log *j){
    /* Check both the requested readonly snapshot and the original bound IO.
     * Taking then refusing the loan would erase somebody else's live frame. */
    return j&&j->readonly&&!j->adapter&&!log_busy(j)&&
        (!s||(s->log!=j&&!log_busy(s->log)));
}
static int query_acquire(br_flow_owner_store *s,const br_flow_session_log *j){
    if(!query_available(s,j))return BR_INVALID;
    int rc=acquire(s);if(rc!=1)return rc;
    /* This image occupies the original wire until AFTER the last callback.
     * Admission alone cannot protect an otherwise idle original producer. */
    br_flow_session_log *original=s->log;
    if(original){
        original->busy=1;
        if(original->adapter){
            original->adapter->busy=1;
            if(original->adapter->session)original->adapter->session->busy=1;
        }
    }
    return 1;
}
static int query_finish(br_flow_owner_store *s,int rc){
    /* Return the borrowed bytes before allowing original IO to run again.
     * Callbacks may not mutate bindings; these are the guards acquired above. */
    memset(s->workspace,0,BR_FLOW_OWNER_HANDOFF_BYTES);
    br_flow_session_log *original=s->log;
    if(original){
        if(original->adapter){
            if(original->adapter->session)original->adapter->session->busy=0;
            original->adapter->busy=0;
        }
        original->busy=0;
    }
    s->busy=0;return rc;
}
int br_flow_owner_store_result(br_flow_owner_store *s,br_flow_session_log *j,unsigned index,
    br_inbox *q,const br_result *identity,const br_result **out){
    if(!out)return BR_INVALID;
    *out=NULL;
    int rc=query_acquire(s,j);if(rc!=1)return rc;
    size_t n=0;rc=load(s,&n);
    if(rc==1){
        br_flow_owner_handoff view={s->workspace,n,s->current,s->context,0};
        rc=br_flow_session_ledger_result(j,index,q,identity,br_flow_owner_handoff_associate,&view,out);
    }else if(!rc)rc=BR_INVALID;
    return query_finish(s,rc);
}
typedef struct {br_flow_owner_store *store;int rc;} audit_attempt;
static int audit_snapshot(void *p,const br_flow_session_log *original){
    audit_attempt *v=p;br_flow_owner_store *s=v->store;
    size_t n=0;unsigned published=0;
    int rc=load(s,&n);
    if(!rc){v->rc=BR_OWNER_MISSING;return 1;}
    if(rc<0){v->rc=rc;return 1;}
    unsigned prepared=original->phase==BR_FLOW_SESSION_LOG_BOUND?0:original->index+1;
    if(br_flow_owner_handoff_prefix(s->workspace,n,&original->binding,original->steps,
           prepared,&published)!=1||s->current(s->context,s->workspace,n)!=1)v->rc=BR_INVALID;
    else if(published==prepared)v->rc=BR_OWNER_ALIGNED;
    else if(prepared==published+1&&original->phase==BR_FLOW_PREPARE)v->rc=BR_OWNER_LOG_AHEAD;
    else if(published==prepared+1)v->rc=BR_OWNER_IMAGE_AHEAD;
    else v->rc=BR_INVALID;
    return 1;
}
int br_flow_owner_store_audit(br_flow_owner_store *s,br_flow_session_log *j){
    int rc=query_acquire(s,j);if(rc!=1)return rc;
    /* Keep the readonly snapshot guarded through its image/current callback,
     * instead of returning the recovery guard before checking the image. */
    audit_attempt attempt={s,BR_IO};
    rc=br_flow_session_log_inspect(j,audit_snapshot,&attempt);
    return query_finish(s,rc==1?attempt.rc:BR_IO);
}
