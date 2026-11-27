#include "schedule_owner.h"
#include <string.h>
_Static_assert(BR_SCHEDULE_OWNER_BYTES<=BR_FLOW_SESSION_WIRE,"original wire loan fits");
static int uuid(const char *s){
    if(!s||s[36]||s[14]!='7'||!(s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b'))return 0;
    for(unsigned i=0;i<36;i++){
        unsigned c=(unsigned char)s[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }return 1;
}
static int bindings(const br_flow_log_binding *b,const br_flow_session_log_binding *v,const char *command){
    return br_flow_log_binding_valid(b)&&br_flow_session_log_binding_valid(v)&&uuid(command)&&
        v->source==BR_LOG_SCHEDULED&&!v->ctl_run&&!v->cycle&&!v->job&&
        b->silent==!b->audible&&b->run==v->run&&b->count==v->count&&
        !strcmp(b->script,v->script)&&!strcmp(b->revision,v->revision)&&
        !strcmp(b->definition_sha256,v->definition_sha256)&&!strcmp(b->inputs_sha256,v->inputs_sha256);
}
/* Explicit endian bytes, no struct padding/host ABI; nested codec stays v1. */
static void header(unsigned char *p,const br_flow_log_binding *b,const char *command){
    memcpy(p,"LX06SCH1",8);p[8]=1;p[9]=p[10]=p[11]=0;
    for(unsigned i=0;i<8;i++)p[12+i]=(unsigned char)(b->serial>>(8*i));
    memset(p+20,0,33);memcpy(p+20,b->task,strlen(b->task));
    memcpy(p+53,command,37);p[90]=(unsigned char)b->silent;p[91]=(unsigned char)b->audible;
}
int br_schedule_owner_encode(const br_flow_log_binding *b,const br_flow_session_log_binding *v,
    const char command[37],const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count,
    void *output,size_t capacity,size_t *length){
    if(length)*length=0;
    if(!output||capacity!=BR_SCHEDULE_OWNER_BYTES)return BR_INVALID;
    memset(output,0,capacity);size_t n=0;
    if(!length||!bindings(b,v,command)||br_flow_owner_handoff_encode(v,steps,ids,count,
        (unsigned char *)output+BR_SCHEDULE_OWNER_HEADER,BR_FLOW_OWNER_HANDOFF_BYTES,&n)!=1){
        memset(output,0,capacity);return BR_INVALID;
    }
    header(output,b,command);*length=BR_SCHEDULE_OWNER_HEADER+n;return 1;
}
int br_schedule_owner_prefix(const void *bytes,size_t length,const br_flow_log_binding *b,
    const br_flow_session_log_binding *v,const char command[37],
    const br_flow_session_log_step *steps,unsigned count,unsigned *published){
    if(!published)return BR_INVALID;
    *published=0;
    if(!bytes||length<BR_SCHEDULE_OWNER_HEADER||length>BR_SCHEDULE_OWNER_BYTES||
       !bindings(b,v,command))return BR_INVALID;
    unsigned char expected[BR_SCHEDULE_OWNER_HEADER];header(expected,b,command);
    if(memcmp(bytes,expected,sizeof expected))return BR_INVALID;
    return br_flow_owner_handoff_prefix((const unsigned char *)bytes+BR_SCHEDULE_OWNER_HEADER,
        length-BR_SCHEDULE_OWNER_HEADER,v,steps,count,published);
}
int br_schedule_owner_transition(const void *bytes,size_t length,unsigned event,
    const br_flow_log_binding *b,const br_flow_session_log_binding *v,const char command[37],
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count){
    if(!bindings(b,v,command))return BR_INVALID;
    if(event==BR_OWNER_INIT)return br_flow_owner_handoff_transition(bytes,length,event,v,steps,ids,count);
    unsigned published=0;
    if(br_schedule_owner_prefix(bytes,length,b,v,command,steps,count,&published)!=1)return BR_INVALID;
    return br_flow_owner_handoff_transition((const unsigned char *)bytes+BR_SCHEDULE_OWNER_HEADER,
        length-BR_SCHEDULE_OWNER_HEADER,event,v,steps,ids,count);
}
int br_schedule_owner_apply(void *bytes,size_t capacity,size_t *length,unsigned event,
    const br_flow_log_binding *b,const br_flow_session_log_binding *v,const char command[37],
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count){
    if(capacity!=BR_SCHEDULE_OWNER_BYTES||!length||
       (event!=BR_OWNER_PREPARE&&event!=BR_OWNER_ASSOCIATE)||
       br_schedule_owner_transition(bytes,*length,event,b,v,command,steps,ids,count)!=1)return BR_INVALID;
    unsigned char *inner=(unsigned char *)bytes+BR_SCHEDULE_OWNER_HEADER;
    size_t n=*length-BR_SCHEDULE_OWNER_HEADER;
    /* All encoder arguments were fully validated by the original transition;
     * immutable/disjoint inputs guarantee encoding cannot subsequently refuse.
     * PREPARE uses original append to retain every prior association byte. */
    int rc=event==BR_OWNER_PREPARE?
        br_flow_owner_handoff_append_prepare(inner,BR_FLOW_OWNER_HANDOFF_BYTES,&n,v,steps,count):
        br_flow_owner_handoff_encode(v,steps,ids,count,inner,BR_FLOW_OWNER_HANDOFF_BYTES,&n);
    if(rc==1)*length=BR_SCHEDULE_OWNER_HEADER+n;
    return rc;
}
static int acquire(br_flow_owner_store *o,size_t capacity){
    if(!o||!o->store||!o->workspace||!o->current||o->busy||o->log||
       capacity!=BR_SCHEDULE_OWNER_BYTES||o->store->schedule!=3||
       o->store->directory<0||o->store->lock<0)return BR_INVALID;
    if(o->blocked||o->store->poisoned)return BR_IO;
    o->busy=1;return 1;
}
static int finish(br_flow_owner_store *o,int rc){
    memset(o->workspace,0,BR_SCHEDULE_OWNER_BYTES);o->busy=0;return rc;
}
static int read_image(br_flow_owner_store *o,size_t *n){
    int rc=br_store_read(o->store,o->workspace,BR_SCHEDULE_OWNER_BYTES,n);
    if(rc<0){o->blocked=1;return BR_IO;}return rc;
}
int br_schedule_owner_seed(br_flow_owner_store *o,size_t capacity,const br_schedule *s,unsigned slot,
    const br_flow *f,const br_flow_log_binding *b,const br_flow_session_log_binding *v,
    const char command[37],br_flow_owner_handoff_current previous,void *context){
    if(!previous||br_schedule_session_check(s,slot,f,b,v,command)!=1)return BR_INVALID;
    int rc=acquire(o,capacity);if(rc!=1)return rc;
    size_t n=0;rc=read_image(o,&n);
    if(rc!=0)return finish(o,rc<0?rc:BR_INVALID);
    if(previous(context,NULL,0)!=1||br_schedule_owner_encode(b,v,command,NULL,NULL,0,
        o->workspace,capacity,&n)!=1||o->current(o->context,o->workspace,n)!=1)
        return finish(o,BR_INVALID);
    /* Original single-writer lock/immutable callbacks are mandatory; check
     * absence again so a changed predecessor is never blindly overwritten. */
    rc=read_image(o,&n);
    if(rc!=0){o->blocked=1;return finish(o,BR_IO);}
    if(br_schedule_owner_encode(b,v,command,NULL,NULL,0,o->workspace,capacity,&n)!=1)
        return finish(o,BR_INVALID);
    if(br_store_write(o->store,o->workspace,n)){o->blocked=1;return finish(o,BR_IO);}
    return finish(o,1);
}
int br_schedule_owner_inspect(br_flow_owner_store *o,size_t capacity,const br_flow_log_binding *b,
    const br_flow_session_log_binding *v,const char command[37],
    const br_flow_session_log_step *steps,unsigned count,unsigned *published){
    if(!published)return BR_INVALID;
    *published=0;
    if(!bindings(b,v,command)||count>v->count||(count&&!steps))return BR_INVALID;
    int rc=acquire(o,capacity);if(rc!=1)return rc;
    size_t n=0;rc=read_image(o,&n);unsigned found=0;
    if(rc==1){
        rc=br_schedule_owner_prefix(o->workspace,n,b,v,command,steps,count,&found);
        if(rc==1&&o->current(o->context,o->workspace,n)!=1)rc=BR_INVALID;
        if(rc==1)*published=found;
    }
    return finish(o,rc);
}
int br_schedule_owner_prepare(br_flow_owner_store *o,br_flow_session_log *j,
    const br_flow_log_binding *b,const char command[37]){
    if(!o||!j||!j->busy||!j->adapter||!j->adapter->busy||!j->adapter->session||
       !j->adapter->session->busy||j->readonly||j->blocked||j->phase!=BR_FLOW_PREPARE||
       !bindings(b,&j->binding,command)||j->adapter->wire!=o->workspace)return BR_INVALID;
    int rc=acquire(o,BR_SCHEDULE_OWNER_BYTES);if(rc!=1)return rc;
    size_t n=0;unsigned count=j->index+1;
    rc=read_image(o,&n);
    unsigned published=0;
    if(rc!=1||br_schedule_owner_prefix(o->workspace,n,b,&j->binding,command,j->steps,count,&published)!=1||
       count!=published+1)goto bad;
    int proof=o->current(o->context,o->workspace,n);
    if(br_flow_session_log_live_current(j)!=1||proof!=1)goto bad;
    size_t inner=n-BR_SCHEDULE_OWNER_HEADER;
    if(br_flow_owner_handoff_append_prepare((unsigned char *)o->workspace+BR_SCHEDULE_OWNER_HEADER,
        BR_FLOW_OWNER_HANDOFF_BYTES,&inner,&j->binding,j->steps,count)!=1)goto bad;
    n=BR_SCHEDULE_OWNER_HEADER+inner;
    proof=o->current(o->context,o->workspace,n);
    if(br_flow_session_log_live_current(j)!=1||proof!=1)goto bad;
    if(br_store_write(o->store,o->workspace,n))goto bad;
    return finish(o,1);
bad:
    o->blocked=j->blocked=1;return finish(o,BR_IO);
}
typedef struct {
    br_flow_owner_store *owner;br_flow_session_log *log;br_flow_session *adapter;
    const br_flow_log_binding *old,*next;const char *old_command,*next_command;
    const br_flow_session_log_binding *binding;
    br_flow_session_log_binding old_binding;br_flow_session_log_step old_steps[BR_FLOW_STEPS];
    br_flow_owner_handoff_current previous;void *previous_context;
    br_flow_session_log_closed closed;void *closed_context;
    unsigned char *image;size_t length;unsigned old_count;int rc;
} scheduled_turnover;
static int turnover_before(void *p,const br_flow_session_log *j){
    scheduled_turnover *x=p;br_flow_owner_store *o=x->owner;size_t n=0;unsigned count=0;
    if(read_image(o,&n)!=1||br_schedule_owner_prefix(o->workspace,n,x->old,&j->binding,x->old_command,
       j->steps,x->old_count,&count)!=1||count!=x->old_count||
       x->previous(x->previous_context,o->workspace,n)!=1||o->current(o->context,o->workspace,n)!=1||
       br_flow_session_log_target_current(x->log,x->adapter,x->binding)!=1)return -1;
    memcpy(x->image,o->workspace,n);x->length=n;return 1;
}
static int turnover_closed(void *p,const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,unsigned count){
    scheduled_turnover *x=p;return x->closed(x->closed_context,b,steps,count);
}
static int turnover_after(void *p,const br_flow_session_log *j){
    scheduled_turnover *x=p;br_flow_owner_store *o=x->owner;size_t n=0;
    /* Log is now NEW BOUND. Old bytes and old identities remain separate. */
    if(!x->length||read_image(o,&n)!=1||n!=x->length||memcmp(o->workspace,x->image,n)||
       x->previous(x->previous_context,o->workspace,n)!=1||
       x->closed(x->closed_context,&x->old_binding,x->old_steps,x->old_count)!=1||
       br_flow_session_log_target_current(x->log,x->adapter,&j->binding)!=1)goto bad;
    if(br_schedule_owner_encode(x->next,&j->binding,x->next_command,NULL,NULL,0,o->workspace,BR_SCHEDULE_OWNER_BYTES,&n)!=1||
       o->current(o->context,o->workspace,n)!=1||
       br_flow_session_log_target_current(x->log,x->adapter,&j->binding)!=1)goto bad;
    if(read_image(o,&n)!=1||n!=x->length||memcmp(o->workspace,x->image,n)||
       br_schedule_owner_encode(x->next,&j->binding,x->next_command,NULL,NULL,0,o->workspace,BR_SCHEDULE_OWNER_BYTES,&n)!=1||
       br_flow_session_log_target_current(x->log,x->adapter,&j->binding)!=1)goto bad;
    if(br_store_write(o->store,o->workspace,n))goto bad;
    x->rc=1;return 1;
bad:o->blocked=1;x->rc=BR_IO;return -1;
}
static int owner_successor(br_flow_owner_store *o,br_flow_session_log *j,br_flow_session *a,
    const br_flow_log_binding *old,const char old_command[37],const br_flow_log_binding *next,
    const br_flow_session_log_binding *binding,const char next_command[37],br_flow_owner_handoff_current previous,
    void *previous_context,br_flow_session_log_closed closed,void *closed_context,
    br_flow_checkpoint hook,void *hook_context,void *old_image,size_t capacity,int kind){
    if(!o||!j||j->busy||j->blocked||j->readonly||
       j->phase!=(kind==2?BR_FLOW_SESSION_LOG_FAILURE_RETIRED:kind==1?BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT:BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED)||
       !a||a->busy||!a->session||a->session->busy||a->wire!=o->workspace||
       !previous||!closed||!hook||!old_image||capacity<BR_SCHEDULE_OWNER_BYTES||old_image==o->workspace||
       !bindings(old,&j->binding,old_command)||!bindings(next,binding,next_command))return BR_INVALID;
    int rc=acquire(o,BR_SCHEDULE_OWNER_BYTES);if(rc!=1)return rc;
    memset(old_image,0,capacity);
    scheduled_turnover x={.owner=o,.log=j,.adapter=a,.old=old,.next=next,.old_command=old_command,.next_command=next_command,
        .binding=binding,.old_binding=j->binding,.previous=previous,.previous_context=previous_context,
        .closed=closed,.closed_context=closed_context,.image=old_image,.old_count=kind==1?0:j->index+1,.rc=BR_INVALID};
    memcpy(x.old_steps,j->steps,sizeof x.old_steps);
    br_flow_session_log_rebind_ops ops={turnover_before,turnover_after,turnover_closed,hook,&x,hook_context};
    rc=(kind==2?br_flow_session_log_rebind_failure_inspected:kind==1?br_flow_session_log_rebind_unsent_inspected:br_flow_session_log_rebind_scheduled_inspected)(j,binding,a,&ops);
    if(rc!=1&&(j->blocked||j->store->poisoned)){o->blocked=1;x.rc=BR_IO;}
    memset(old_image,0,capacity);memset(&x.old_binding,0,sizeof x.old_binding);memset(x.old_steps,0,sizeof x.old_steps);
    return finish(o,rc==1?1:x.rc);
}
int br_schedule_owner_turnover(br_flow_owner_store *o,br_flow_session_log *j,br_flow_session *a,
    const br_flow_log_binding *old,const char old_command[37],const br_flow_log_binding *next,
    const br_flow_session_log_binding *binding,const char next_command[37],br_flow_owner_handoff_current previous,
    void *previous_context,br_flow_session_log_closed closed,void *closed_context,
    br_flow_checkpoint hook,void *hook_context,void *old_image,size_t capacity){
    return owner_successor(o,j,a,old,old_command,next,binding,next_command,previous,previous_context,closed,closed_context,hook,hook_context,old_image,capacity,0);
}
int br_schedule_owner_retry(br_flow_owner_store *o,br_flow_session_log *j,br_flow_session *a,
    const br_flow_log_binding *old,const char old_command[37],const br_flow_log_binding *next,
    const br_flow_session_log_binding *binding,const char next_command[37],br_flow_owner_handoff_current previous,
    void *previous_context,br_flow_session_log_closed closed,void *closed_context,
    br_flow_checkpoint hook,void *hook_context,void *old_image,size_t capacity){
    return owner_successor(o,j,a,old,old_command,next,binding,next_command,previous,previous_context,closed,closed_context,hook,hook_context,old_image,capacity,1);
}
int br_schedule_owner_after_failure(br_flow_owner_store *o,br_flow_session_log *j,br_flow_session *a,
    const br_flow_log_binding *old,const char old_command[37],const br_flow_log_binding *next,
    const br_flow_session_log_binding *binding,const char next_command[37],br_flow_owner_handoff_current previous,
    void *previous_context,br_flow_session_log_closed closed,void *closed_context,
    br_flow_checkpoint hook,void *hook_context,void *old_image,size_t capacity){
    return owner_successor(o,j,a,old,old_command,next,binding,next_command,previous,previous_context,closed,closed_context,hook,hook_context,old_image,capacity,2);
}

/* The old full image has already passed scheduled prefix/current provenance.
 * This callback adds no authority: it only lets the original pure codec test
 * whether this exact row is already known. */
static int codec_current(void *p,const void *bytes,size_t n){(void)p;(void)bytes;(void)n;return 1;}
static int associated(const unsigned char *bytes,size_t n,const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,unsigned index,const br_result *id){
    br_flow_owner_handoff h={bytes+BR_SCHEDULE_OWNER_HEADER,n-BR_SCHEDULE_OWNER_HEADER,codec_current,NULL,0};
    return br_flow_owner_handoff_associate(&h,b,index,steps+index,id);
}
int br_schedule_owner_associate(br_flow_owner_store *o,br_flow_session_log *j,const br_flow_log_binding *b,
    const char command[37],unsigned index,const br_result *id,int (*current)(void *),void *context,
    void *old_image,size_t capacity){
    if(!o||!j||!j->busy||j->readonly||j->blocked||j->phase!=BR_FLOW_PREPARE||index!=j->index||
       !j->adapter||!j->adapter->busy||!j->adapter->session||!j->adapter->session->busy||
       j->adapter->wire!=o->workspace||!bindings(b,&j->binding,command)||
       !br_result_valid(id)||id->code[0]||id->text[0]||!current||!old_image||old_image==o->workspace||
       capacity<BR_SCHEDULE_OWNER_BYTES)return BR_INVALID;
    int rc=acquire(o,BR_SCHEDULE_OWNER_BYTES);if(rc!=1)return rc;
    memset(old_image,0,capacity);size_t n=0,old_n=0;unsigned published=0,count=j->index+1;
    if(read_image(o,&n)!=1||br_schedule_owner_prefix(o->workspace,n,b,&j->binding,command,j->steps,count,&published)!=1||
       published!=count||current(context)!=1||o->current(o->context,o->workspace,n)!=1||current(context)!=1)goto bad;
    memcpy(old_image,o->workspace,n);old_n=n;
    if(associated(o->workspace,n,&j->binding,j->steps,index,id)==1){
        if(read_image(o,&n)!=1||n!=old_n||memcmp(o->workspace,old_image,n)||
           current(context)!=1||o->current(o->context,o->workspace,n)!=1||current(context)!=1)goto bad;
        memset(old_image,0,capacity);return finish(o,1); /* exact observation; zero writes */
    }
    size_t inner=n-BR_SCHEDULE_OWNER_HEADER;
    if(br_flow_owner_handoff_associate_step((unsigned char *)o->workspace+BR_SCHEDULE_OWNER_HEADER,
       BR_FLOW_OWNER_HANDOFF_BYTES,&inner,&j->binding,j->steps,count,index,id)!=1)goto bad;
    n=BR_SCHEDULE_OWNER_HEADER+inner;
    if(current(context)!=1||o->current(o->context,o->workspace,n)!=1||current(context)!=1)goto bad;
    if(read_image(o,&n)!=1||n!=old_n||memcmp(o->workspace,old_image,n)||current(context)!=1)goto bad;
    inner=n-BR_SCHEDULE_OWNER_HEADER;
    if(br_flow_owner_handoff_associate_step((unsigned char *)o->workspace+BR_SCHEDULE_OWNER_HEADER,
       BR_FLOW_OWNER_HANDOFF_BYTES,&inner,&j->binding,j->steps,count,index,id)!=1)goto bad;
    n=BR_SCHEDULE_OWNER_HEADER+inner;
    if(o->current(o->context,o->workspace,n)!=1||current(context)!=1||br_store_write(o->store,o->workspace,n))goto bad;
    inner=old_n-BR_SCHEDULE_OWNER_HEADER;
    if(br_flow_owner_handoff_associate_step((unsigned char *)old_image+BR_SCHEDULE_OWNER_HEADER,
       BR_FLOW_OWNER_HANDOFF_BYTES,&inner,&j->binding,j->steps,count,index,id)!=1)goto bad;
    old_n=BR_SCHEDULE_OWNER_HEADER+inner;
    if(current(context)!=1||read_image(o,&n)!=1||n!=old_n||memcmp(o->workspace,old_image,n)||
       o->current(o->context,o->workspace,n)!=1||current(context)!=1)goto bad;
    memset(old_image,0,capacity);return finish(o,1);
bad:o->blocked=j->blocked=1;memset(old_image,0,capacity);return finish(o,BR_IO);
}
int br_schedule_owner_associated(br_flow_owner_store *o,const br_flow_log_binding *b,
    const br_flow_session_log_binding *v,const char command[37],const br_flow_session_log_step *steps,
    unsigned count,unsigned index,const br_result *id){
    if(!bindings(b,v,command)||!steps||!count||index>=count||count>v->count||
       !br_result_valid(id)||id->code[0]||id->text[0])return BR_INVALID;
    int rc=acquire(o,BR_SCHEDULE_OWNER_BYTES);if(rc!=1)return rc;
    size_t n=0;unsigned published=0;rc=read_image(o,&n);
    if(rc==1){
        rc=br_schedule_owner_prefix(o->workspace,n,b,v,command,steps,count,&published);
        if(rc==1&&published==count&&associated(o->workspace,n,v,steps,index,id)==1&&
           o->current(o->context,o->workspace,n)==1)rc=1;else rc=BR_INVALID;
    }
    return finish(o,rc);
}

typedef struct {
    br_flow_owner_store *owner;const void *image;size_t length;br_flow_owner_handoff handoff;
    const br_inbox *inbox;br_store *store;int directory,lock;
} scheduled_ledger_borrow;
/* Read-only lifetime check, matching the original inbox owner contract. A
 * cached RAM row cannot outlive its exact original namespace/lock handle.
 * No descriptor IO, new receipt, recovery or authority is performed here. */
static int borrowed_store_current(const scheduled_ledger_borrow *v){
    const br_inbox *q=v->inbox;const br_store *store=v->store;
    return q&&q->store==store&&!q->blocked&&store&&!store->poisoned&&!store->schedule&&
        v->directory>=0&&v->lock>=0&&store->directory==v->directory&&store->lock==v->lock;
}
static int borrowed_provenance(scheduled_ledger_borrow *v){
    if(!borrowed_store_current(v))return -1;
    int proof=v->owner->current(v->owner->context,v->image,v->length);
    return borrowed_store_current(v)&&proof==1?1:-1;
}
static int borrowed_current(void *p,const void *bytes,size_t length){
    scheduled_ledger_borrow *v=p;
    return bytes==v->handoff.bytes&&length==v->handoff.length&&v->owner->busy&&
        borrowed_provenance(v)==1?1:-1;
}
static int borrowed_associate(void *p,const br_flow_session_log_binding *b,unsigned index,
    const br_flow_session_log_step *step,const br_result *id){
    scheduled_ledger_borrow *v=p;return br_flow_owner_handoff_associate(&v->handoff,b,index,step,id);
}
int br_schedule_owner_ledger_result(br_flow_owner_store *o,const br_flow_log_binding *b,
    br_flow_session_log *j,const char command[37],unsigned index,br_inbox *q,const br_result *id,const br_result **result){
    if(!result)return BR_INVALID;*result=NULL;
    if(!o||!j||j->busy||!j->readonly||j->blocked||!j->phase||j->phase==BR_FLOW_SESSION_LOG_BOUND||
       index>j->index||index>=j->binding.count||!bindings(b,&j->binding,command)||
       !q||!br_result_valid(id)||id->code[0]||id->text[0])return BR_INVALID;
    scheduled_ledger_borrow v={.owner=o,.inbox=q,.store=q->store,
        .directory=q->store?q->store->directory:-1,.lock=q->store?q->store->lock:-1};
    if(!borrowed_store_current(&v))return BR_IO;
    const br_flow_session_log *snapshot=NULL;
    int recovered=br_flow_session_log_recovery(j,&snapshot);
    if(!borrowed_store_current(&v))return BR_IO;
    if(recovered!=1||snapshot!=j)return j->blocked?BR_IO:BR_INVALID;
    int rc=acquire(o,BR_SCHEDULE_OWNER_BYTES);if(rc!=1)return rc;
    size_t n=0;unsigned count=j->index+1,published=0;rc=read_image(o,&n);
    if(rc==1){
        v.image=o->workspace;v.length=n;
        if(br_schedule_owner_prefix(o->workspace,n,b,&j->binding,command,j->steps,count,&published)!=1||
           published!=count||associated(o->workspace,n,&j->binding,j->steps,index,id)!=1||
           borrowed_provenance(&v)!=1)rc=BR_INVALID;
        else{
            v.handoff=(br_flow_owner_handoff){(unsigned char *)o->workspace+BR_SCHEDULE_OWNER_HEADER,
                n-BR_SCHEDULE_OWNER_HEADER,borrowed_current,&v,0};
            /* Loading IO is complete. The mandatory original associate uses
             * the immutable original codec/provenance only, never owner IO.
             * Keep the locked owner and exact bytes borrowed through K6. */
            rc=br_flow_session_ledger_result(j,index,q,id,borrowed_associate,&v,result);
            /* The original pure owner callback runs before original K6's
             * independent associate; guard that same store immediately before
             * it and again after the original query returns. */
            if(!borrowed_store_current(&v)){*result=NULL;rc=BR_IO;}
            else if(borrowed_provenance(&v)!=1){*result=NULL;rc=borrowed_store_current(&v)?BR_INVALID:BR_IO;}
            memset(&v,0,sizeof v);
        }
    }
    return finish(o,rc);
}
