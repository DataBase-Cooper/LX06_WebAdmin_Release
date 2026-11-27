#include "flow_session_log.h"
#include "result_fingerprint.h"
#include <string.h>
_Static_assert(sizeof(br_flow_session_log)+BR_FLOW_SESSION_LOG_BYTES+sizeof(br_store)<4096,
    "bounded identity log, store and independent IO workspace");
/* Fixed, explicit little endian schema9. No JSON/settings parser or ABI image. */
typedef struct { unsigned char *p;size_t n; } cursor;
static void bytes(cursor *c,void *p,size_t n,int write){
    if(write)memcpy(c->p+c->n,p,n);else memcpy(p,c->p+c->n,n);c->n+=n;
}
static uint64_t number(cursor *c,uint64_t v,unsigned n,int write){
    if(write)for(unsigned i=0;i<n;i++)c->p[c->n+i]=(unsigned char)(v>>(i*8));
    else{v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)c->p[c->n+i]<<(i*8);}
    c->n+=n;return v;
}
static int text(const char *s,size_t capacity,int hash){
    size_t n=0;
    for(;n<capacity&&s[n];n++){
        unsigned v=(unsigned char)s[n];
        if(hash?!((v>='0'&&v<='9')||(v>='a'&&v<='f')):
           !((v>='a'&&v<='z')||(v>='A'&&v<='Z')||(v>='0'&&v<='9')||v=='_'||v=='-'||v=='.'))return 0;
    }
    if(!n||n==capacity||(hash&&n!=64))return 0;
    while(++n<capacity)if(s[n])return 0;
    return 1;
}
static int uuid(const char *s){
    if(s[36]||s[14]!='7'||!(s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b'))return 0;
    for(unsigned i=0;i<36;i++){
        unsigned v=(unsigned char)s[i];
        if(i==8||i==13||i==18||i==23){if(v!='-')return 0;}
        else if(!((v>='0'&&v<='9')||(v>='a'&&v<='f')))return 0;
    }return 1;
}
static int valid(const br_flow_session_log_binding *b){
    return b&&text(b->script,sizeof(b->script),0)&&text(b->flow_sha256,sizeof(b->flow_sha256),1)&&
        text(b->revision,sizeof(b->revision),1)&&text(b->definition_sha256,sizeof(b->definition_sha256),1)&&
        text(b->inputs_sha256,sizeof(b->inputs_sha256),1)&&b->run&&b->session_run&&b->epoch&&b->generation&&
        ((!b->ctl_run&&!b->cycle&&!b->job)||(b->ctl_run&&b->cycle&&b->job))&&
        b->count&&b->count<=BR_FLOW_STEPS&&b->source>=BR_LOG_OFFICIAL&&b->source<=BR_LOG_SCHEDULED;
}
int br_flow_session_log_binding_valid(const br_flow_session_log_binding *b){return valid(b);}
static int binding(cursor *c,const br_flow_session_log_binding *b,int write){
    const char *strings[]={b->script,b->flow_sha256,b->revision,b->definition_sha256,b->inputs_sha256};
    const size_t sizes[]={129,65,65,65,65};
    for(unsigned i=0;i<5;i++){
        if(write)bytes(c,(void *)strings[i],sizes[i],1);
        else{if(memcmp(c->p+c->n,strings[i],sizes[i]))return 0;c->n+=sizes[i];}
    }
    const uint64_t values[]={b->run,b->session_run,b->epoch,b->generation,b->ctl_run,b->cycle,b->job,b->count,b->source};
    for(unsigned i=0;i<9;i++)if(number(c,values[i],i<7?8:1,write)!=values[i])return 0;
    return 1;
}
/* 4 schema + 389 strings + 56 identities + 2 count/source + 2 phase/index. */
#define HEADER 453u
#define STEP 45u
_Static_assert(HEADER+STEP*BR_FLOW_STEPS+40<=BR_FLOW_SESSION_LOG_BYTES,"schema fits workspace");
static int failure_phase(unsigned phase){return phase==BR_FLOW_SESSION_LOG_SCHEDULED_FAILED||phase==BR_FLOW_SESSION_LOG_FAILURE_RETIRED;}
static int failure_code(const char *code){
    static const char *const known[]={"bridge_unreachable","token_invalid","bridge_connect_timeout","app_not_installed",
        "hook_unavailable","hook_version_mismatch","app_version_unsupported","bridge_version_mismatch",
        "session_missing","search_empty","op_timeout","script_disabled","unsupported","quota_exhausted"};
    if(!text(code,40,0))return 0;
    for(unsigned i=0;i<sizeof known/sizeof *known;i++)if(!strcmp(code,known[i]))return 1;
    return 0;
}
static int decode(br_flow_session_log *j,size_t n){
    if(n<HEADER)return 0;
    cursor c={j->workspace,0};
    uint64_t schema=number(&c,0,4,0);
    if((schema!=9&&schema!=10&&schema!=11&&schema!=12)||!binding(&c,&j->binding,0))return 0;
    j->phase=(unsigned)number(&c,0,1,0);j->index=(unsigned)number(&c,0,1,0);
    if(schema==11)return j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT&&
        j->binding.source==BR_LOG_SCHEDULED&&!j->binding.ctl_run&&!j->binding.cycle&&
        !j->binding.job&&!j->index&&n==c.n;
    if(j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT)return 0;
    if(schema==12){
        if(!failure_phase(j->phase)||j->binding.source!=BR_LOG_SCHEDULED||
           j->binding.ctl_run||j->binding.cycle||j->binding.job)return 0;
    }else if(failure_phase(j->phase))return 0;
    if(schema==10){
        if(j->phase!=BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED||j->binding.source!=BR_LOG_SCHEDULED||
           j->binding.ctl_run||j->binding.cycle||j->binding.job||j->index+1!=j->binding.count)return 0;
    }else if(j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED)return 0;
    if(j->phase==BR_FLOW_SESSION_LOG_BOUND)return !j->index&&n==c.n;
    if((schema==10||schema==12?0:j->phase==BR_FLOW_SESSION_LOG_RETIRED||j->phase==BR_FLOW_SESSION_LOG_RETIRED_LIVE?
        (j->binding.source==BR_LOG_SCHEDULED||j->index+1!=j->binding.count):
        (j->phase<BR_FLOW_PREPARE||j->phase>BR_FLOW_REAPED))||j->index>=j->binding.count||
       n!=c.n+STEP*(j->index+1)+(schema==12?40:0))return 0;
    for(unsigned i=0;i<=j->index;i++){
        br_flow_session_log_step *s=j->steps+i;s->ticket=number(&c,0,8,0);bytes(&c,s->command,37,0);
        if(!s->ticket||!uuid(s->command)||(i&&s->ticket<=j->steps[i-1].ticket))return 0;
        for(unsigned k=0;k<i;k++)if(!strcmp(s->command,j->steps[k].command))return 0;
    }
    if(schema==12){bytes(&c,j->failure_code,40,0);if(!failure_code(j->failure_code))return 0;}
    return c.n==n;
}
static int healthy(const br_flow_session_log *j){
    return j&&!j->busy&&!j->blocked&&j->store&&!j->store->poisoned&&j->store->schedule==2&&
        j->workspace&&j->owner&&valid(&j->binding);
}
int br_flow_session_log_open(br_flow_session_log *j,br_store *s,const br_flow_session_log_binding *b,
    void *workspace,size_t capacity,br_flow_session_log_owner owner,void *context){
    if(!j||!s||s->schedule!=2||!valid(b)||!workspace||capacity!=BR_FLOW_SESSION_LOG_BYTES||!owner)return -1;
    memset(j,0,sizeof(*j));j->store=s;j->binding=*b;j->workspace=workspace;j->owner=owner;j->context=context;
    size_t n=0;int rc=br_store_read(s,workspace,BR_FLOW_SESSION_LOG_BYTES,&n);
    if(rc==1){if(!decode(j,n))rc=-1;else j->readonly=1;}
    if(rc<0){j->blocked=1;memset(j->steps,0,sizeof(j->steps));}
    memset(workspace,0,BR_FLOW_SESSION_LOG_BYTES);return rc;
}
static int matches(const br_flow_session *a,const br_flow *f,const br_flow_session_log_binding *b,int closed){
    static const char *const sources[]={"","official","local","scheduled"};
    return a&&a->bound&&a->flow==f&&f&&f->run==b->run&&a->run==b->run&&a->session_run==b->session_run&&
        a->epoch==b->epoch&&a->generation==b->generation&&f->session_epoch==b->epoch&&
        f->session_generation==b->generation&&a->session->run==b->session_run&&a->session->epoch==b->epoch&&
        a->session->generation<=b->generation&&a->session->operation.discovery&&
        (a->session->operation.discovery->epoch==b->epoch||
         (closed&&!a->session->operation.discovery->epoch))&&
        f->definition&&f->definition->count==b->count&&f->script_id&&!strcmp(f->script_id,b->script)&&
        f->wake_source&&!strcmp(f->wake_source,sources[b->source]);
}
static int current(const br_flow_session_log *j,const br_flow *f){return matches(j->adapter,f,&j->binding,0);}
static int write_log(br_flow_session_log *j,const br_flow_session_log_binding *b,
                     const br_flow *f,unsigned phase,unsigned index){
    cursor c={j->workspace,0};
    (void)number(&c,failure_phase(phase)?12:phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT?11:phase==BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED?10:9,4,1);(void)binding(&c,b,1);
    (void)number(&c,phase,1,1);(void)number(&c,index,1,1);
    if(phase!=BR_FLOW_SESSION_LOG_BOUND&&phase!=BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT)for(unsigned i=0;i<=index;i++){
        (void)number(&c,f&&i==index?f->ticket:j->steps[i].ticket,8,1);
        bytes(&c,(void *)(f?f->records[i].command:j->steps[i].command),37,1);
    }
    if(failure_phase(phase))bytes(&c,j->failure_code,40,1);
    if(br_store_write(j->store,j->workspace,c.n)){j->blocked=1;return -1;}
    return 1;
}
static int save(br_flow_session_log *j,const br_flow *f,unsigned phase,unsigned index){
    if(write_log(j,&j->binding,f,phase,index)!=1)return -1;
    j->phase=phase;j->index=index;
    if(phase!=BR_FLOW_SESSION_LOG_BOUND){j->steps[index].ticket=f->ticket;memcpy(j->steps[index].command,f->command,37);}
    return 1;
}
/* New admission requires the original connection AFTER the proof, including
 * a refusing proof. Success/reap and readonly recovery keep their existing
 * semantics: a lost connection must not erase an already admitted identity. */
static int admission_proven(br_flow_session_log *j,int proof){
    int live=br_flow_session_log_live_current(j)==1;
    return live&&proof==1;
}
int br_flow_session_log_attach(br_flow_session_log *j,br_flow_session *a){
    if(!healthy(j)||j->readonly||j->phase||j->adapter||!a||a->busy||!a->bound||!a->flow||!a->session)return -1;
    br_flow *f=a->flow;
    if(f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->used||f->checkpoint||
       a->active||!a->session->live||a->session->busy||a->session->owned)return -1;
    j->adapter=a;
    if(!current(j,f)){j->adapter=NULL;return -1;}
    j->busy=a->busy=a->session->busy=1;int rc=-1;size_t present=0;
    /* An absent open is not permission to overwrite a file created before
     * attach. Keep the singleton writer lock, recheck absence at this barrier. */
    if(br_store_read(j->store,j->workspace,BR_FLOW_SESSION_LOG_BYTES,&present)==0&&
       admission_proven(j,j->owner(j->context,&j->binding))&&save(j,NULL,BR_FLOW_SESSION_LOG_BOUND,0)==1&&
       br_flow_set_session_checkpoint(f,j->binding.run,br_flow_session_log_checkpoint,j)==1)rc=1;
    if(rc!=1)j->blocked=1;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);a->session->busy=a->busy=j->busy=0;return rc;
}
static int disk_current(br_flow_session_log *j);
int br_flow_session_log_set_prepare(br_flow_session_log *j,br_flow_checkpoint prepared,void *context){
    if(!healthy(j)||j->readonly||j->phase!=BR_FLOW_SESSION_LOG_BOUND||j->prepared||
       !prepared||!j->adapter)return -1;
    br_flow_session *a=j->adapter;br_flow *f=a->flow;
    if(a->busy||a->active||!f||!a->session||a->session->busy||!a->session->live||
       a->session->owned||!current(j,f)||f->status!=BR_FLOW_NEXT||f->index||f->ticket||
       f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j)return -1;
    j->busy=a->busy=a->session->busy=1;int rc=-1;
    if(disk_current(j)==1&&admission_proven(j,j->owner(j->context,&j->binding))){
        j->prepared=prepared;j->prepared_context=context;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);a->session->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_checkpoint(void *p,const br_flow *f,unsigned event){
    br_flow_session_log *j=p;
    if(!healthy(j)||j->readonly||!j->phase||j->phase==BR_FLOW_SESSION_LOG_RETIRED||
       j->phase==BR_FLOW_SESSION_LOG_RETIRED_LIVE||j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED||
       j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT||failure_phase(j->phase))return -1;
    br_flow_session *a=j->adapter;
    /* The adapter normally owns busy across dispatch/receive/poll, but the
     * session does not. Protect both through ALL proof/extension callbacks.
     * A direct legacy checkpoint also acquires adapter busy; restore exactly
     * its caller's guard, and never borrow a session already held elsewhere. */
    if(!a||!a->session||a->session->busy)return -1;
    br_session *s=a->session;int adapter_busy=a->busy;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(!current(j,f)||f->local||f->oneway||!f->ticket||f->used>BR_FLOW_ARENA||
       event<BR_FLOW_PREPARE||event>BR_FLOW_REAPED||(!f->index&&event==BR_FLOW_REAPED))goto done;
    unsigned index=event==BR_FLOW_REAPED?f->index-1:f->index;
    if(index>=j->binding.count||f->epoch!=j->binding.epoch||!uuid(f->command)||
       f->records[index].epoch!=f->epoch||strcmp(f->records[index].command,f->command))goto done;
    if((event==BR_FLOW_PREPARE&&(f->status!=BR_FLOW_NEXT||!j->adapter->session->live))||
       (event==BR_FLOW_SUCCESS&&(f->status!=BR_FLOW_WAIT||!j->adapter->session->live))||
       (event==BR_FLOW_REAPED&&f->status!=BR_FLOW_DRAINING))goto done;
    for(unsigned i=0;i<index;i++)if(f->records[i].epoch!=j->binding.epoch||
        strcmp(f->records[i].command,j->steps[i].command)||!j->steps[i].ticket||j->steps[i].ticket>=f->ticket)goto done;
    if(event==BR_FLOW_PREPARE){
        if(f->status!=BR_FLOW_NEXT||!((j->phase==BR_FLOW_SESSION_LOG_BOUND&&!index)||
           (j->phase==BR_FLOW_REAPED&&index==j->index+1)))goto duplicate;
        for(unsigned i=0;i<index;i++)if(!strcmp(f->command,j->steps[i].command))goto done;
    }else{
        if(index!=j->index||f->ticket!=j->steps[index].ticket||strcmp(f->command,j->steps[index].command))goto done;
        if(event==BR_FLOW_SUCCESS&&(f->status!=BR_FLOW_WAIT||j->phase!=BR_FLOW_PREPARE))goto duplicate;
        if(event==BR_FLOW_REAPED&&(f->status!=BR_FLOW_DRAINING||j->phase!=BR_FLOW_SUCCESS))goto duplicate;
    }
    if(j->prepared&&disk_current(j)!=1)goto done;
    int proof=j->owner(j->context,&j->binding);
    if(event==BR_FLOW_PREPARE?!admission_proven(j,proof):proof!=1)goto done;
    rc=save(j,f,event,index);
    if(rc==1&&event==BR_FLOW_PREPARE&&j->prepared)
        /* A generic extension may observe revocation while publishing its
         * own durable prefix. Its strict1 is not renewed send authority. */
        rc=admission_proven(j,j->prepared(j->prepared_context,f,event))?1:-1;
    goto done;
duplicate:
    /* An already admitted legacy PREPARE is comparison only. Its ticket may
     * still be owned; do not apply a NEW-admission/unowned requirement here. */
    if(j->phase==event&&j->index==index&&j->steps[index].ticket==f->ticket&&
       !strcmp(j->steps[index].command,f->command)&&!j->prepared&&j->owner(j->context,&j->binding)==1)rc=1;
done:
    if(rc!=1)j->blocked=1;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=0;a->busy=adapter_busy;j->busy=0;return rc;
}
int br_flow_session_log_recovery(br_flow_session_log *j,const br_flow_session_log **out){
    if(!out)return -1;
    *out=NULL;if(!healthy(j)||!j->readonly||!j->phase)return -1;
    j->busy=1;int rc=disk_current(j)==1&&j->owner(j->context,&j->binding)==1?1:-1;
    if(rc==1)*out=j;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);
    j->busy=0;return rc;
}
int br_flow_session_log_inspect(br_flow_session_log *j,br_flow_session_log_visitor visit,void *context){
    if(!healthy(j)||!j->phase||!visit)return -1;
    br_flow_session *a=j->adapter;br_session *s=NULL;
    if(j->readonly){if(a)return -1;}
    else{
        if(!a||a->busy||!a->bound||!a->flow||!a->session||a->session->busy||
           !matches(a,a->flow,&j->binding,1)||
           a->flow->checkpoint!=br_flow_session_log_checkpoint||a->flow->checkpoint_context!=j)return -1;
        s=a->session;
    }
    j->busy=1;if(a){a->busy=1;s->busy=1;}
    int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1)
        rc=visit(context,j)==1?1:-1;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);
    if(a){s->busy=0;a->busy=0;}j->busy=0;return rc;
}
/* Re-read exact committed snapshot before replacement. Never decode into the
 * cached handle: a foreign/corrupt file must not change its original identity. */
static int disk_current(br_flow_session_log *j){
    int empty=j->phase==BR_FLOW_SESSION_LOG_BOUND||j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT;
    size_t n=0,expected=HEADER+(empty?0:STEP*(j->index+1))+(failure_phase(j->phase)?40:0);
    if(br_store_read(j->store,j->workspace,BR_FLOW_SESSION_LOG_BYTES,&n)!=1||n!=expected)goto bad;
    cursor c={j->workspace,0};
    if(number(&c,0,4,0)!=(failure_phase(j->phase)?12u:j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT?11u:j->phase==BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED?10u:9u)||!binding(&c,&j->binding,0)||
       number(&c,0,1,0)!=j->phase||number(&c,0,1,0)!=j->index)goto bad;
    if(!empty)for(unsigned i=0;i<=j->index;i++){
        if(number(&c,0,8,0)!=j->steps[i].ticket||memcmp(c.p+c.n,j->steps[i].command,37))goto bad;
        c.n+=37;
    }
    if(failure_phase(j->phase)&&(memcmp(c.p+c.n,j->failure_code,40)||!failure_code(j->failure_code)))goto bad;
    return 1;
bad:j->blocked=1;return -1;
}
/* Current suffix is the original FLOW's actual admitted session commands.
 * Neither historical REAPED nor unowned alone proves this association. */
static int session_suffix(const br_flow_session_log *j,const br_session *s){
    const br_flow_session_log_binding *b=&j->binding;
    if(!s||s->busy||s->owned||s->run!=b->session_run||s->epoch!=b->epoch||
       s->generation!=b->generation||s->count<b->count||s->count>BR_SESSION_LIMIT||
       strcmp(s->script,b->script)||!s->operation.discovery)return 0;
    for(unsigned i=0;i<b->count;i++){
        const br_session_ticket *t=s->tickets+s->count-b->count+i;
        if(t->generation!=b->generation||strcmp(t->command,j->steps[i].command))return 0;
    }
    return 1;
}
static int live_session(const br_flow_session_log *j,const br_session *s){
    if(!session_suffix(j,s)||!s->live||s->ended)return 0;
    const br_discovery *d=s->operation.discovery;
    return d->epoch==j->binding.epoch&&d->phase==BR_DISC_READY&&d->caps.ready&&
        d->caps.connection==j->binding.epoch;
}
/* Busy guards refuse nested close/disconnect APIs, but a synchronous owner
 * may still discover and revoke the original discovery authority. Recheck
 * after EACH live-handoff callback, including a refusing callback. Never
 * publish a new retirement/binding from a proof obtained on a lost session.
 * Ignore busy here: these are precisely the guards held by this operation.
 * Binding/adapter identities remain immutable for the duration of callbacks. */
static int session_proof(br_flow_session_log *j,const br_session *s,
    const br_flow_session_log_binding *b,int proof){
    if(!s)return proof==1; /* Existing closed/recovered retirement semantics. */
    const br_discovery *d=s->operation.discovery;
    if(!s->live||s->ended||s->owned||s->run!=b->session_run||s->epoch!=b->epoch||
       !d||d->epoch!=b->epoch||d->phase!=BR_DISC_READY||d->describing||!d->caps.ready||
       d->caps.connection!=b->epoch){j->blocked=1;return 0;}
    return proof==1;
}
static int live_proof(br_flow_session_log *j,const br_session *s,int proof){
    return session_proof(j,s,&j->binding,proof);
}
int br_flow_session_log_live_current(br_flow_session_log *j){
    if(!j||j->readonly||j->blocked||!j->busy||!j->adapter||!j->adapter->busy||
       !j->adapter->session||!j->adapter->session->busy)return -1;
    return live_proof(j,j->adapter->session,1)?1:-1;
}
int br_flow_session_log_target_current(br_flow_session_log *j,br_flow_session *a,
    const br_flow_session_log_binding *b){
    if(!j||!j->busy||j->blocked||!a||!a->busy||!a->session||!a->session->busy||!valid(b))return -1;
    if(!matches(a,a->flow,b,0)){j->blocked=1;return -1;}
    return session_proof(j,a->session,b,1)?1:-1;
}
static int target_proven(br_flow_session_log *j,br_flow_session *a,
    const br_flow_session_log_binding *b,int proof){
    int current=br_flow_session_log_target_current(j,a,b)==1;
    return current&&proof==1;
}
static int retirement_proven(br_flow_session_log *j,br_flow_session_log_owner released,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context,const br_session *live){
    if(visit&&!live_proof(j,live,visit(context,j)))return 0;
    return live_proof(j,live,closed?closed(context,&j->binding,j->steps,j->binding.count):
        released(context,&j->binding));
}
static int retire_mode(br_flow_session_log *j,br_flow_session_log_owner released,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context,int live){
    if(!healthy(j)||j->readonly||(!released&&!closed)||j->binding.source==BR_LOG_SCHEDULED||
       j->phase!=BR_FLOW_REAPED||j->index+1!=j->binding.count||!j->adapter)return -1;
    br_flow_session *a=j->adapter;br_flow *f=a->flow;br_session *s=a->session;
    if(a->busy||a->active||s->busy||s->owned||(live?!live_session(j,s):s->live)||
       !matches(a,f,&j->binding,!live)||
       f->status!=BR_FLOW_DONE||f->index!=j->binding.count||
       f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j)return -1;
    for(unsigned i=0;i<j->binding.count;i++)if(f->records[i].epoch!=j->binding.epoch||
       strcmp(f->records[i].command,j->steps[i].command))return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    unsigned phase=live?BR_FLOW_SESSION_LOG_RETIRED_LIVE:BR_FLOW_SESSION_LOG_RETIRED;
    if(disk_current(j)==1&&live_proof(j,live?s:NULL,j->owner(j->context,&j->binding))&&
       retirement_proven(j,released,visit,closed,context,live?s:NULL)&&
       write_log(j,&j->binding,NULL,phase,j->index)==1){
        j->phase=phase;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_retire(br_flow_session_log *j,br_flow_session_log_owner released,void *context){
    return retire_mode(j,released,NULL,NULL,context,0);
}
int br_flow_session_log_retire_scheduled(br_flow_session_log *j,br_flow_session_log_visitor visit,void *context){
    if(!healthy(j)||j->readonly||!visit||j->binding.source!=BR_LOG_SCHEDULED||
       j->binding.ctl_run||j->binding.cycle||j->binding.job||j->phase!=BR_FLOW_REAPED||
       j->index+1!=j->binding.count||!j->adapter)return -1;
    br_flow_session *a=j->adapter;br_flow *f=a->flow;br_session *s=a->session;
    if(a->busy||a->active||!s||!session_suffix(j,s)||s->live||
       !matches(a,f,&j->binding,1)||f->status!=BR_FLOW_DONE||f->index!=j->binding.count||
       f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j)return -1;
    for(unsigned i=0;i<j->binding.count;i++)if(f->records[i].epoch!=j->binding.epoch||
       strcmp(f->records[i].command,j->steps[i].command))return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&visit(context,j)==1&&
       !s->live&&!s->owned&&!a->active&&matches(a,f,&j->binding,1)&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED,j->index)==1){
        j->phase=BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_retire_live(br_flow_session_log *j,br_flow_session_log_owner released,void *context){
    return retire_mode(j,released,NULL,NULL,context,1);
}
static int unsent_shape(br_flow_session_log *j,int closing){
    br_flow_session *a=j->adapter;br_flow *f=a?a->flow:NULL;br_session *s=a?a->session:NULL;
    return a&&s&&f&&!a->active&&!a->ticket&&!a->slot&&!s->owned&&!s->count&&!s->generation&&
        !s->script[0]&&!s->terminal&&!f->index&&!f->ticket&&!f->used&&!f->command[0]&&!f->request_length&&
        !f->deadline_expired&&f->status==(closing?BR_FLOW_CANCELLED:BR_FLOW_NEXT)&&
        matches(a,f,&j->binding,closing)&&f->checkpoint==br_flow_session_log_checkpoint&&f->checkpoint_context==j&&
        (!closing||!s->live);
}
static int unsent_admit(br_flow_session_log *j){
    return healthy(j)&&!j->readonly&&j->phase==BR_FLOW_SESSION_LOG_BOUND&&!j->index&&
        j->binding.source==BR_LOG_SCHEDULED&&!j->binding.ctl_run&&!j->binding.cycle&&!j->binding.job&&
        j->adapter&&!j->adapter->busy&&j->adapter->session&&!j->adapter->session->busy;
}
int br_flow_session_log_cancel_unsent(br_flow_session_log *j,uint64_t now,
    br_flow_session_log_visitor visit,void *context){
    if(!unsent_admit(j)||!visit||!unsent_shape(j,0)||now<j->adapter->flow->now_ms||
       now-j->adapter->flow->started_ms>=900000)return -1;
    br_flow_session *a=j->adapter;br_session *s=a->session;j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&visit(context,j)==1&&unsent_shape(j,0))
        rc=br_flow_poll(a->flow,j->binding.run,now,1,&s->operation.discovery->scratch)==BR_FLOW_CANCELLED?1:-1;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_retire_unsent(br_flow_session_log *j,br_flow_session_log_visitor visit,void *context){
    if(!unsent_admit(j)||!visit||!unsent_shape(j,1))return -1;
    br_flow_session *a=j->adapter;br_session *s=a->session;j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&visit(context,j)==1&&unsent_shape(j,1)&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT,0)==1){
        j->phase=BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
static int failed_prefix(br_flow_session_log *j,int closed){
    br_flow_session *a=j->adapter;br_flow *f=a?a->flow:NULL;br_session *s=a?a->session:NULL;
    unsigned count=j->index+1;
    if(!a||!s||!f||j->index>=j->binding.count||!matches(a,f,&j->binding,closed)||
       s->generation!=j->binding.generation||s->count<count||s->count>BR_SESSION_LIMIT||
       s->count!=a->slot+1||strcmp(s->script,j->binding.script)||a->ticket!=j->steps[j->index].ticket||
       f->index!=j->index||f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j)return 0;
    for(unsigned i=0;i<count;i++){
        const br_session_ticket *t=s->tickets+s->count-count+i;
        if(t->generation!=j->binding.generation||strcmp(t->command,j->steps[i].command))return 0;
    }
    return s->tickets[a->slot].result_type==BR_WIRE_ERROR;
}
static int original_failure(br_flow_session_log *j){
    if(!failed_prefix(j,0))return 0;
    br_flow_session *a=j->adapter;br_session *s=a->session;br_flow *f=a->flow;br_operation *op=&s->operation;
    if(!a->active||!s->live||!s->owned||!s->terminal||f->status!=BR_FLOW_WAIT||f->ticket!=a->ticket||
       op->last_event!=BR_OP_ERROR||op->pending||!op->error||strlen(op->error)>=40||
       op->epoch!=j->binding.epoch||strcmp(op->command_id,j->steps[j->index].command)||
       strcmp(op->task_id,s->tickets[a->slot].task)||br_json_kind(&op->response,1)!=BR_J_OBJECT)return 0;
    unsigned char hash[32];br_result_fingerprint(&op->response,hash);
    int same=!memcmp(hash,s->tickets[a->slot].result_hash,sizeof hash);memset(hash,0,sizeof hash);return same;
}
static int failure_admit(br_flow_session_log *j){
    return healthy(j)&&!j->readonly&&j->binding.source==BR_LOG_SCHEDULED&&
        !j->binding.ctl_run&&!j->binding.cycle&&!j->binding.job&&j->adapter&&!j->adapter->busy&&
        j->adapter->session&&!j->adapter->session->busy;
}
int br_flow_session_log_record_failure(br_flow_session_log *j,br_flow_session_log_visitor visit,void *context){
    if(!failure_admit(j)||!visit||j->phase!=BR_FLOW_PREPARE||!original_failure(j))return -1;
    br_flow_session *a=j->adapter;br_session *s=a->session;j->busy=a->busy=s->busy=1;int rc=-1;
    memset(j->failure_code,0,sizeof j->failure_code);strcpy(j->failure_code,s->operation.error);
    memcpy(j->failure_hash,s->tickets[a->slot].result_hash,sizeof j->failure_hash);
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&failure_code(j->failure_code)&&
       visit(context,j)==1&&original_failure(j)&&!strcmp(j->failure_code,s->operation.error)&&
       !memcmp(j->failure_hash,s->tickets[a->slot].result_hash,sizeof j->failure_hash)&&disk_current(j)==1&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_SCHEDULED_FAILED,j->index)==1){
        j->phase=BR_FLOW_SESSION_LOG_SCHEDULED_FAILED;
        if(visit(context,j)==1&&original_failure(j)&&disk_current(j)==1)rc=1;
        else j->blocked=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_retire_failure(br_flow_session_log *j,br_flow_session_log_visitor visit,void *context){
    if(!failure_admit(j)||!visit||j->phase!=BR_FLOW_SESSION_LOG_SCHEDULED_FAILED||!failed_prefix(j,1))return -1;
    br_flow_session *a=j->adapter;br_session *s=a->session;br_flow *f=a->flow;
    if(a->active||s->live||s->owned||f->status!=BR_FLOW_FAILED||!f->error||strcmp(f->error,j->failure_code)||
       memcmp(j->failure_hash,s->tickets[a->slot].result_hash,sizeof j->failure_hash))return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&visit(context,j)==1&&
       failed_prefix(j,1)&&!a->active&&!s->live&&!s->owned&&
       !memcmp(j->failure_hash,s->tickets[a->slot].result_hash,sizeof j->failure_hash)&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_FAILURE_RETIRED,j->index)==1){
        j->phase=BR_FLOW_SESSION_LOG_FAILURE_RETIRED;
        if(visit(context,j)==1&&disk_current(j)==1)rc=1;else j->blocked=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
static int close_retired_live(br_flow_session_log *j,br_flow_session_log_owner released,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context){
    if(!healthy(j)||j->readonly||(!released&&!closed)||j->binding.source==BR_LOG_SCHEDULED||
       j->phase!=BR_FLOW_SESSION_LOG_RETIRED_LIVE||j->index+1!=j->binding.count||!j->adapter)return -1;
    br_flow_session *a=j->adapter;br_flow *f=a->flow;br_session *s=a->session;
    if(a->busy||a->active||!f||!session_suffix(j,s)||s->live||s->terminal||
       (s->operation.discovery->epoch&&s->operation.discovery->epoch!=j->binding.epoch))return -1;
    if(a->bound){
        if(!matches(a,f,&j->binding,1)||f->status!=BR_FLOW_DONE||f->index!=j->binding.count||
           f->checkpoint!=br_flow_session_log_checkpoint||f->checkpoint_context!=j)return -1;
        for(unsigned i=0;i<j->binding.count;i++)if(f->records[i].epoch!=j->binding.epoch||
           strcmp(f->records[i].command,j->steps[i].command))return -1;
    }else if(a->run||a->session_run||a->epoch||a->generation||a->ticket||a->slot||
             f->status!=BR_FLOW_IDLE||f->run||f->checkpoint||f->checkpoint_context||f->used||f->ticket)return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&
       retirement_proven(j,released,visit,closed,context,NULL)&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_RETIRED,j->index)==1){
        j->phase=BR_FLOW_SESSION_LOG_RETIRED;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_close_retired_live(br_flow_session_log *j,br_flow_session_log_owner released,void *context){
    return close_retired_live(j,released,NULL,NULL,context);
}
static int inspect_scheduled_closed(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context,int kind){
    if(!healthy(j)||!visit||!closed||(kind&&j->readonly)||
       j->phase!=(kind==2?BR_FLOW_SESSION_LOG_FAILURE_RETIRED:kind==1?BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT:BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED)||
       j->binding.source!=BR_LOG_SCHEDULED||j->binding.ctl_run||j->binding.cycle||j->binding.job||
       (kind==2?j->index>=j->binding.count:kind==1?j->index!=0:j->index+1!=j->binding.count))return -1;
    br_flow_session *a=j->adapter;br_session *s=a?a->session:NULL;
    /* Old adapter may have been legitimately cleared for a fresh connection.
     * The full CURRENT old resource proof remains external and mandatory. */
    if(a&&(a->busy||a->bound||a->active||!s||s->busy))return -1;
    j->busy=1;if(a)a->busy=s->busy=1;
    int rc=disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&visit(context,j)==1&&
        closed(context,&j->binding,j->steps,kind==1?0:j->index+1)==1?1:-1;
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);
    if(a){s->busy=a->busy=0;}j->busy=0;return rc;
}
int br_flow_session_log_inspect_scheduled_retired(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context){
    return inspect_scheduled_closed(j,visit,closed,context,0);
}
int br_flow_session_log_inspect_unsent(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context){
    return inspect_scheduled_closed(j,visit,closed,context,1);
}
int br_flow_session_log_inspect_failure_retired(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context){
    return inspect_scheduled_closed(j,visit,closed,context,2);
}
static int rebind_closed(br_flow_session_log *j,const br_flow_session_log_binding *b,
    br_flow_session *a,br_flow_session_log_owner released,br_flow_session_log_closed closed,void *context,
    const br_flow_session_log_rebind_ops *ops,int scheduled){
    if(!healthy(j)||(j->prepared&&!ops)||!valid(b)||(!released&&!closed)||
       j->phase!=(scheduled==3?BR_FLOW_SESSION_LOG_FAILURE_RETIRED:scheduled==2?BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT:scheduled?BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED:BR_FLOW_SESSION_LOG_RETIRED)||
       (scheduled==3?(j->readonly||j->index>=j->binding.count):scheduled==2?(j->readonly||j->index!=0):j->index!=j->binding.count-1)||
       (scheduled?(!ops||!closed||j->binding.source!=BR_LOG_SCHEDULED||b->source!=BR_LOG_SCHEDULED||
           j->binding.ctl_run||j->binding.cycle||j->binding.job||b->ctl_run||b->cycle||b->job):
           (j->binding.source==BR_LOG_SCHEDULED||b->source==BR_LOG_SCHEDULED))||
       b->run<=j->binding.run||b->session_run<=j->binding.session_run||
       b->epoch<=j->binding.epoch||b->generation<=j->binding.generation||
       (j->adapter&&j->adapter!=a&&(j->adapter->busy||j->adapter->bound||j->adapter->active))||
       !a||a->busy||!a->bound||a->active||!a->flow||!a->session)return -1;
    br_flow *f=a->flow;br_session *s=a->session;
    if(s->busy||!s->live||s->owned||s->ended||!matches(a,f,b,0)||f->status!=BR_FLOW_NEXT||
       f->index||f->ticket||f->used||f->checkpoint)return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(target_proven(j,a,b,1)&&disk_current(j)==1&&(!ops||
       (target_proven(j,a,b,j->owner(j->context,&j->binding))&&
        target_proven(j,a,b,ops->before(context,j))))&&
       target_proven(j,a,b,closed?closed(context,&j->binding,j->steps,scheduled==2?0:j->index+1):released(context,&j->binding))&&
       target_proven(j,a,b,j->owner(j->context,b))&&
       write_log(j,b,NULL,BR_FLOW_SESSION_LOG_BOUND,0)==1){
        j->binding=*b;j->adapter=a;j->readonly=0;j->phase=BR_FLOW_SESSION_LOG_BOUND;j->index=0;
        memset(j->steps,0,sizeof(j->steps));
        memset(j->failure_code,0,sizeof j->failure_code);memset(j->failure_hash,0,sizeof j->failure_hash);
        rc=br_flow_set_session_checkpoint(f,b->run,br_flow_session_log_checkpoint,j);
        if(ops){
            j->prepared=NULL;j->prepared_context=NULL;
            if(rc==1)rc=target_proven(j,a,b,ops->after(context,j))?1:-1;
            if(rc==1){j->prepared=ops->prepared;j->prepared_context=ops->prepared_context;}
        }
        if(rc!=1)j->blocked=1;
    }
    /* A failed log write must not leave the fresh FLOW with an unguarded
     * legacy dispatch path. This checkpoint only refuses; it cannot send. */
    if(rc!=1&&j->blocked&&!f->checkpoint&&(ops||!session_proof(j,s,b,1)))
        (void)br_flow_set_session_checkpoint(f,b->run,br_flow_session_log_checkpoint,j);
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_rebind(br_flow_session_log *j,const br_flow_session_log_binding *b,
    br_flow_session *a,br_flow_session_log_owner released,void *context){
    return rebind_closed(j,b,a,released,NULL,context,NULL,0);
}
int br_flow_session_log_rebind_closed(br_flow_session_log *j,const br_flow_session_log_binding *b,
    br_flow_session *a,br_flow_session_log_closed closed,void *context){
    return rebind_closed(j,b,a,NULL,closed,context,NULL,0);
}
int br_flow_session_log_rebind_inspected(br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,const br_flow_session_log_rebind_ops *ops){
    if(!ops||!ops->before||!ops->after||!ops->closed||!ops->prepared||
       (j&&j->readonly&&j->adapter))return -1;
    return rebind_closed(j,b,a,NULL,ops->closed,ops->context,ops,0);
}
int br_flow_session_log_rebind_scheduled_inspected(br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,const br_flow_session_log_rebind_ops *ops){
    if(!ops||!ops->before||!ops->after||!ops->closed||!ops->prepared||
       (j&&j->readonly&&j->adapter))return -1;
    return rebind_closed(j,b,a,NULL,ops->closed,ops->context,ops,1);
}
int br_flow_session_log_rebind_unsent_inspected(br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,const br_flow_session_log_rebind_ops *ops){
    if(!ops||!ops->before||!ops->after||!ops->closed||!ops->prepared)return -1;
    return rebind_closed(j,b,a,NULL,ops->closed,ops->context,ops,2);
}
int br_flow_session_log_rebind_failure_inspected(br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,const br_flow_session_log_rebind_ops *ops){
    if(!ops||!ops->before||!ops->after||!ops->closed||!ops->prepared)return -1;
    return rebind_closed(j,b,a,NULL,ops->closed,ops->context,ops,3);
}
static int recover_closed(br_flow_session_log *j,br_flow_session_log_visitor visit,
    br_flow_session_log_closed closed,void *context){
    if(!healthy(j)||!j->readonly||j->adapter||!closed||j->binding.source==BR_LOG_SCHEDULED||
       j->phase!=BR_FLOW_SESSION_LOG_RETIRED_LIVE||j->index!=j->binding.count-1)return -1;
    j->busy=1;int rc=-1;
    if(disk_current(j)==1&&j->owner(j->context,&j->binding)==1&&
       retirement_proven(j,NULL,visit,closed,context,NULL)&&
       write_log(j,&j->binding,NULL,BR_FLOW_SESSION_LOG_RETIRED,j->index)==1){
        j->phase=BR_FLOW_SESSION_LOG_RETIRED;rc=1;
    }
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);j->busy=0;return rc;
}
int br_flow_session_log_recover_closed(br_flow_session_log *j,br_flow_session_log_closed closed,void *context){
    return recover_closed(j,NULL,closed,context);
}
int br_flow_session_log_retire_inspected(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_closed closed,void *context){
    if(!j||!visit||!closed)return -1;
    if(j->readonly)return recover_closed(j,visit,closed,context);
    if(j->phase==BR_FLOW_SESSION_LOG_RETIRED_LIVE)
        return close_retired_live(j,NULL,visit,closed,context);
    return retire_mode(j,NULL,visit,closed,context,0);
}
int br_flow_session_log_retire_live_inspected(br_flow_session_log *j,
    br_flow_session_log_visitor visit,br_flow_session_log_handed_off handed_off,void *context){
    if(!visit||!handed_off)return -1;
    return retire_mode(j,NULL,visit,handed_off,context,1);
}
static int rebind_live(br_flow_session_log *j,const br_flow_session_log_binding *b,
    br_flow_session *a,br_flow_session_log_owner released,void *context,
    const br_flow_session_log_turnover_ops *ops){
    if(!healthy(j)||(j->prepared&&!ops)||j->readonly||!valid(b)||(!released&&!ops)||j->phase!=BR_FLOW_SESSION_LOG_RETIRED_LIVE||
       j->binding.source==BR_LOG_SCHEDULED||b->source==BR_LOG_SCHEDULED||
       b->run<=j->binding.run||b->generation<=j->binding.generation||
       b->session_run!=j->binding.session_run||b->epoch!=j->binding.epoch||
       strcmp(b->script,j->binding.script)||strcmp(b->revision,j->binding.revision)||
       b->ctl_run!=j->binding.ctl_run||
       (b->ctl_run?b->cycle<=j->binding.cycle:(b->cycle||b->job))||
       !a||a!=j->adapter||a->busy||!a->bound||a->active||!a->flow||!a->session)return -1;
    br_flow *f=a->flow;br_session *s=a->session;
    if(!live_session(j,s)||s->count>BR_SESSION_LIMIT-b->count||!matches(a,f,b,0)||
       f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->used||f->checkpoint)return -1;
    j->busy=a->busy=s->busy=1;int rc=-1;
    if(disk_current(j)==1&&(!ops||(live_proof(j,s,j->owner(j->context,&j->binding))&&
       live_proof(j,s,ops->before(context,j))))&&
       live_proof(j,s,ops?ops->handed_off(context,&j->binding,j->steps,j->binding.count):released(context,&j->binding))&&
       live_proof(j,s,j->owner(j->context,b))&&
       write_log(j,b,NULL,BR_FLOW_SESSION_LOG_BOUND,0)==1){
        j->binding=*b;j->phase=BR_FLOW_SESSION_LOG_BOUND;j->index=0;
        memset(j->steps,0,sizeof(j->steps));
        rc=br_flow_set_session_checkpoint(f,b->run,br_flow_session_log_checkpoint,j);
        if(ops){
            j->prepared=NULL;j->prepared_context=NULL;
            if(rc==1)rc=live_proof(j,s,ops->after(context,j))?1:-1;
            if(rc==1){j->prepared=ops->prepared;j->prepared_context=ops->prepared_context;}
        }
        if(rc!=1)j->blocked=1;
    }
    /* Legacy same-session handoffs need the same revocation barrier as a
     * fresh-session rebind. Returning caps cannot turn a failed durable
     * handoff into an unguarded dispatch. Preserve unrelated legacy IO rules. */
    if(rc!=1&&j->blocked&&!f->checkpoint&&(ops||!live_proof(j,s,1)))
        (void)br_flow_set_session_checkpoint(f,b->run,br_flow_session_log_checkpoint,j);
    memset(j->workspace,0,BR_FLOW_SESSION_LOG_BYTES);s->busy=a->busy=j->busy=0;return rc;
}
int br_flow_session_log_rebind_live(br_flow_session_log *j,const br_flow_session_log_binding *b,
    br_flow_session *a,br_flow_session_log_owner released,void *context){
    return rebind_live(j,b,a,released,context,NULL);
}
int br_flow_session_log_rebind_live_inspected(br_flow_session_log *j,
    const br_flow_session_log_binding *b,br_flow_session *a,const br_flow_session_log_turnover_ops *ops){
    if(!ops||!ops->before||!ops->after||!ops->handed_off||!ops->prepared)return -1;
    return rebind_live(j,b,a,NULL,ops->context,ops);
}
