#include "flow.h"
#include <stdio.h>
#include <string.h>
_Static_assert(sizeof(br_flow)<48*1024,"bounded flow state");
typedef struct {unsigned char *data;size_t length,capacity;int ok;} writer;
static void bytes(writer *w,const void *p,size_t n){
    if(!w->ok||n>w->capacity-w->length){w->ok=0;return;}
    if(n&&w->data)memcpy(w->data+w->length,p,n);
    w->length+=n;
}
static void put(writer *w,const char *s){bytes(w,s,strlen(s));}
static void quoted(writer *w,const unsigned char *s,size_t n){
    static const char hex[]="0123456789abcdef";
    put(w,"\"");
    for(size_t i=0;i<n;i++){
        unsigned c=s[i];
        if(c=='"'||c=='\\'){unsigned char q[2]={'\\',(unsigned char)c};bytes(w,q,2);}
        else if(c<32){char q[6]={'\\','u','0','0',hex[c>>4],hex[c&15]};bytes(w,q,6);}
        else bytes(w,s+i,1);
    }
    put(w,"\"");
}
static void string(writer *w,const char *s){quoted(w,(const unsigned char *)s,strlen(s));}
static void number(writer *w,uint64_t n){char s[21];(void)snprintf(s,sizeof(s),"%llu",(unsigned long long)n);put(w,s);}
static unsigned field(const br_json *d,unsigned n,const char *s){return br_json_field(d,n,s);}
static int ident(const char *s){
    if(!s||!*s)return 0;
    size_t n=0;
    for(;s[n];n++){
        unsigned c=(unsigned char)s[n];
        if(n==32||!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'||(n&&c>='0'&&c<='9')))return 0;
    }
    return 1;
}
static int bounded(const char *s,size_t max){
    if(!s)return 0;
    for(size_t n=0;n<=max;n++)if(!s[n])return 1;
    return 0;
}
static int scalar(const br_json *d,unsigned n){
    int k=br_json_kind(d,n);return k==BR_J_STRING||k==BR_J_INT||k==BR_J_NUMBER||k==BR_J_TRUE||k==BR_J_FALSE||k==BR_J_NULL;
}
static int emit_scalar(writer *w,const br_json *d,unsigned n){
    if(!scalar(d,n))return 0;
    br_text t=br_json_text(d,n);
    if(br_json_kind(d,n)==BR_J_STRING)quoted(w,t.data,t.length);else bytes(w,t.data,t.length);
    return w->ok;
}
/* The only structured value: complete row objects, scalar typed fields.
 * Declaration, cardinality and required-field checks remain in current caps
 * (and A's sole input validator). No recursion, indexing or row splitting. */
static int rows(const br_json *d,unsigned n){
    if(br_json_kind(d,n)!=BR_J_ARRAY||!br_json_child(d,n))return 0;
    for(unsigned row=br_json_child(d,n);row&&row<br_json_next(d,n);row=br_json_next(d,row)){
        if(br_json_kind(d,row)!=BR_J_OBJECT)return 0;
        for(unsigned k=br_json_child(d,row);k&&k<br_json_next(d,row);k=br_json_next(d,k+1)){
            int type=br_json_kind(d,k+1);
            if(!br_json_text(d,k).length||(type!=BR_J_STRING&&type!=BR_J_INT&&type!=BR_J_TRUE&&type!=BR_J_FALSE))return 0;
        }
    }
    return 1;
}
static int emit_value(writer *w,const br_json *d,unsigned n,int lists){
    if(scalar(d,n))return emit_scalar(w,d,n);
    if(!lists||!rows(d,n))return 0;
    br_text t=br_json_text(d,n);bytes(w,t.data,t.length);return w->ok;
}
static int literal(br_json *scratch,const char *s){
    return bounded(s,8192)&&br_json_parse(scratch,s,strlen(s))==1&&(scalar(scratch,1)||rows(scratch,1));
}
static int step_index(const br_flow_definition *d,const char *id,unsigned before){
    if(!ident(id))return -1;
    for(unsigned i=0;i<before;i++)if(!strcmp(d->steps[i].id,id))return (int)i;
    return -1;
}
static int value_valid(const br_flow_definition *d,unsigned before,const br_flow_value *v,br_json *scratch){
    if(v->source==BR_FLOW_LITERAL)return !v->step&&!v->key&&literal(scratch,v->literal);
    if(v->literal)return 0;
    if(v->source==BR_FLOW_INPUT)return !v->step&&ident(v->key);
    if(v->source!=BR_FLOW_JSON&&v->source!=BR_FLOW_TEXT)return 0;
    return step_index(d,v->step,before)>=0&&(v->source==BR_FLOW_JSON?ident(v->key):!v->key);
}
static int bindings_valid(const br_flow_definition *d,unsigned before,const br_flow_binding *b,size_t n,br_json *scratch,int inputs,int lists){
    if(n>16||(n&&!b))return 0;
    for(size_t i=0;i<n;i++){
        if(!ident(b[i].key)||(inputs&&b[i].value.source!=BR_FLOW_LITERAL)||!value_valid(d,before,&b[i].value,scratch))return 0;
        if(!lists&&b[i].value.source==BR_FLOW_LITERAL&&!scalar(scratch,1))return 0;
        for(size_t j=0;j<i;j++)if(!strcmp(b[j].key,b[i].key))return 0;
    }
    return 1;
}
static void wipe_request(br_flow *f){memset(f->request,0,sizeof(f->request));f->request_length=0;}
static void wipe_results(br_flow *f){memset(f->arena,0,sizeof(f->arena));memset(f->records,0,sizeof(f->records));f->used=0;}
static void cleanup(br_flow *f){
    wipe_request(f);memset(f->command,0,sizeof(f->command));memset(f->task,0,sizeof(f->task));
    f->ticket=0;f->acked=0;
    f->status=f->after_drain;
    if(f->status==BR_FLOW_FAILED||f->status==BR_FLOW_CANCELLED)wipe_results(f);
}
static void stop(br_flow *f,enum br_flow_status status,const char *error){
    if(f->status>=BR_FLOW_DONE)return;
    if(f->status==BR_FLOW_DRAINING&&(f->after_drain==BR_FLOW_FAILED||f->after_drain==BR_FLOW_CANCELLED))return;
    f->after_drain=status;f->error=error;
    if(f->status==BR_FLOW_WAIT||f->status==BR_FLOW_DRAINING){
        f->status=BR_FLOW_DRAINING;
        if(!f->cancelled){f->cancelled=1;f->io.cancel(f->io.context,f->ticket,f->now_ms);}
    }else cleanup(f);
}
static int clock_check(br_flow *f,uint64_t run,uint64_t now){
    if(!f||!run||run!=f->run)return 0;
    if(f->status>=BR_FLOW_DONE)return 1;
    if(now<f->now_ms){stop(f,BR_FLOW_FAILED,"bridge_unreachable");return 1;}
    f->now_ms=now;
    /* A finished step still draining cannot release a successor at timeout. */
    if(!(f->status==BR_FLOW_DRAINING&&(f->after_drain==BR_FLOW_FAILED||f->after_drain==BR_FLOW_CANCELLED))&&
       (now-f->started_ms>=900000||(f->status==BR_FLOW_WAIT&&now-f->step_ms>=f->timeout_ms))){
        f->deadline_expired=1;stop(f,BR_FLOW_FAILED,"op_timeout");
    }
    return 1;
}
static int finish_call(br_flow *f,br_json *scratch){if(scratch)memset(scratch,0,sizeof(*scratch));return f?(int)f->status:-1;}
int br_flow_init(br_flow *f,br_flow_io io){
    if(!f||!io.start||!io.cancel||!io.reaped)return -1;
    memset(f,0,sizeof(*f));f->io=io;return 1;
}
int br_flow_clear(br_flow *f){
    if(!f||f->status==BR_FLOW_NEXT||f->status==BR_FLOW_WAIT||f->status==BR_FLOW_DRAINING)return 0;
    br_flow_io io=f->io;uint64_t last=f->last_run,ticket=f->last_ticket;
    memset(f,0,sizeof(*f));f->io=io;f->last_run=last;f->last_ticket=ticket;return 1;
}
static int definition_valid(const br_flow_definition *d,const br_flow_binding *inputs,size_t n,br_json *scratch){
    int ok=d&&d->steps&&d->count&&d->count<=BR_FLOW_STEPS;
    if(ok)ok=bindings_valid(d,0,inputs,n,scratch,1,1);
    for(unsigned i=0;ok&&i<d->count;i++){
        const br_flow_step *s=d->steps+i;
        ok=ident(s->id)&&step_index(d,s->id,i)<0&&bounded(s->action,256)&&*s->action&&
            strncmp(s->action,"flow:",5)&&(!s->target||bounded(s->target,128))&&s->audio<=7&&
            bindings_valid(d,i,s->params,s->param_count,scratch,0,1)&&bindings_valid(d,i,s->set,s->set_count,scratch,0,0);
        if(ok&&!strcmp(s->action,"system.op"))
            ok=bounded(s->peer_op,128)&&*s->peer_op&&s->target&&!strcmp(s->target,"system")&&!s->audio&&!s->set_count;
        else if(ok)ok=!s->peer_op;
    }
    if(ok&&d->output)ok=d->output->source!=BR_FLOW_LITERAL&&value_valid(d,(unsigned)d->count,d->output,scratch);
    return ok;
}
int br_flow_begin(br_flow *f,uint64_t run,uint64_t now,const br_flow_definition *d,
                  const br_flow_binding *inputs,size_t n,const char *script,const char *source,const char *wake,br_json *scratch){
    if(!f||!scratch||!f->io.start||!run||run<=f->last_run||
       (f->status>BR_FLOW_IDLE&&f->status<BR_FLOW_DONE))return -1;
    int ok=bounded(script,128)&&*script&&bounded(source,16)&&
        (!strcmp(source,"official")||!strcmp(source,"local")||!strcmp(source,"scheduled"))&&
        (!wake||bounded(wake,512))&&(!strcmp(source,"scheduled")?(!wake||!*wake):1)&&
        definition_valid(d,inputs,n,scratch);
    memset(scratch,0,sizeof(*scratch));if(!ok)return -1;
    (void)br_flow_clear(f);f->run=f->last_run=run;f->started_ms=f->now_ms=now;
    f->definition=d;f->inputs=inputs;f->input_count=n;f->script_id=script;f->wake_source=source;f->wake_text=wake;
    f->status=BR_FLOW_NEXT;return 1;
}
int br_flow_require_silent(br_flow *f,uint64_t run,const br_system_rule *rules,size_t count){
    if(!f||!run||run!=f->run||f->silent||f->status!=BR_FLOW_NEXT||f->index||
       strcmp(f->wake_source,"scheduled")||count>32||(count&&!rules))return -1;
    for(size_t i=0;i<count;i++){
        if(!bounded(rules[i].op,128)||!*rules[i].op||rules[i].kind!=BR_SYSTEM_ACK||
           !rules[i].timeout_ms||rules[i].timeout_ms>120000)return -1;
        for(size_t j=0;j<i;j++)if(!strcmp(rules[i].op,rules[j].op))return -1;
    }
    f->silent=1;f->silent_rules=rules;f->silent_rule_count=count;return 1;
}
int br_flow_set_checkpoint(br_flow *f,uint64_t run,br_flow_checkpoint checkpoint,void *context){
    if(!f||!run||f->run!=run||f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->used||
       f->checkpoint||f->session_epoch||!checkpoint)return -1;
    f->checkpoint=checkpoint;f->checkpoint_context=context;return 1;
}
int br_flow_set_session_checkpoint(br_flow *f,uint64_t run,br_flow_checkpoint checkpoint,void *context){
    if(!f||!run||f->run!=run||f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->used||
       f->checkpoint||!f->session_epoch||!f->session_generation||!checkpoint)return -1;
    f->checkpoint=checkpoint;f->checkpoint_context=context;return 1;
}
int br_flow_use_session(br_flow *f,uint64_t run,uint64_t epoch,uint64_t generation){
    if(!f||!run||run!=f->run||!epoch||!generation||f->status!=BR_FLOW_NEXT||
       f->index||f->ticket||f->used||f->checkpoint||f->session_epoch||!f->definition)return -1;
    for(size_t i=0;i<f->definition->count;i++){
        const br_flow_step *s=f->definition->steps+i;
        if(!strcmp(s->action,"system.op")||s->audio)return -1;
    }
    f->session_epoch=epoch;f->session_generation=generation;return 1;
}
static int checkpoint(br_flow *f,unsigned event){
    return !f->checkpoint||f->checkpoint(f->checkpoint_context,f,event)==1;
}
static void result_view(const br_flow *f,br_flow_results *v){
    memset(v,0,sizeof(*v));v->definition=f->definition;v->inputs=f->inputs;v->input_count=f->input_count;
    v->arena=f->arena;v->used=f->used;v->count=f->index;
    for(unsigned i=0;i<v->count&&i<BR_FLOW_STEPS;i++)v->records[i]=f->records+i;
}
static int declared(const br_flow_results *f,unsigned index,const char *key,br_json *scratch){
    const br_flow_record *r=f->records[index];
    if(!r->declarations_length||br_json_parse(scratch,f->arena+r->declarations,r->declarations_length)!=1)return 0;
    for(unsigned n=br_json_child(scratch,1);n&&n<br_json_next(scratch,1);n=br_json_next(scratch,n))
        if(br_json_is(scratch,n,key))return 1;
    return 0;
}
static int resolve(const br_flow_results *f,const br_flow_value *v,br_json *scratch,writer *w,int lists){
    if(v->source==BR_FLOW_LITERAL){
        if(!literal(scratch,v->literal))return 0;
        return emit_value(w,scratch,1,lists);
    }
    if(v->source==BR_FLOW_INPUT){
        for(size_t i=0;i<f->input_count;i++)if(!strcmp(f->inputs[i].key,v->key))
            return resolve(f,&f->inputs[i].value,scratch,w,lists);
        return 0;
    }
    int i=step_index(f->definition,v->step,f->count);if(i<0)return 0;
    const br_flow_record *r=f->records[i];
    if(v->source==BR_FLOW_JSON&&!declared(f,(unsigned)i,v->key,scratch))return 0;
    if(!r->result_length||br_json_parse(scratch,f->arena+r->result,r->result_length)!=1)return 0;
    unsigned n=v->source==BR_FLOW_TEXT?field(scratch,1,"text"):field(scratch,field(scratch,1,"json"),v->key);
    return emit_value(w,scratch,n,lists);
}
static int bindings(const br_flow *f,const br_flow_binding *b,size_t n,br_json *scratch,writer *w,int lists){
    br_flow_results view;result_view(f,&view);
    put(w,"{");
    for(size_t i=0;i<n;i++){
        if(i)put(w,",");
        string(w,b[i].key);put(w,":");if(!resolve(&view,&b[i].value,scratch,w,lists))return 0;
    }
    put(w,"}");return w->ok;
}
static int save(br_flow *f,const void *data,size_t n,uint16_t *offset,uint16_t *length){
    if(n>BR_FLOW_ARENA-f->used)return 0;
    *offset=(uint16_t)f->used;*length=(uint16_t)n;memcpy(f->arena+f->used,data,n);f->used+=(unsigned)n;return 1;
}
static int declarations(br_flow *f,const br_caps *caps,const br_caps_plan *plan){
    const br_json *d=&caps->document;br_flow_record *r=f->records+f->index;
    unsigned apps=field(d,1,"apps");
    for(unsigned a=br_json_child(d,apps);a&&a<br_json_next(d,apps);a=br_json_next(d,a)){
        br_text app=br_json_text(d,field(d,a,"app"));
        if(app.length!=plan->app.length||memcmp(app.data,plan->app.data,app.length))continue;
        unsigned ops=field(d,a,"ops");
        for(unsigned o=br_json_child(d,ops);o&&o<br_json_next(d,ops);o=br_json_next(d,o)){
            br_text op=br_json_text(d,field(d,o,"name"));
            if(op.length!=plan->op.length||memcmp(op.data,plan->op.data,op.length))continue;
            unsigned n=field(d,field(d,o,"returns"),"json_fields");
            br_text t=br_json_text(d,n);
            return !n||save(f,t.data,t.length,&r->declarations,&r->declarations_length);
        }
    }
    return 0;
}
int br_flow_dispatch(br_flow *f,uint64_t run,uint64_t ticket,uint64_t epoch,const char *command,
                     uint64_t now,const br_caps *caps,br_json *scratch){
    if(!f||!scratch||!clock_check(f,run,now))return -1;
    if(f->status!=BR_FLOW_NEXT)return finish_call(f,scratch);
    if(!ticket||ticket<=f->last_ticket||!bounded(command,36)||strlen(command)!=36||!epoch||!caps){
        stop(f,BR_FLOW_FAILED,"flow_invalid");return finish_call(f,scratch);
    }
    const br_flow_step *s=f->definition->steps+f->index;int local=!strcmp(s->action,"system.op");
    if(f->session_epoch&&(epoch!=f->session_epoch||!f->session_generation||local||s->audio||
       !br_caps_has_feature(caps,epoch,"session.command_id"))){
        stop(f,BR_FLOW_FAILED,"flow_invalid");return finish_call(f,scratch);
    }
    for(unsigned i=0;i<f->index;i++)if(!strcmp(command,f->records[i].command)||
       (!f->session_epoch&&!local&&f->records[i].epoch==epoch)){
        stop(f,BR_FLOW_FAILED,"flow_invalid");return finish_call(f,scratch);
    }
    wipe_request(f);writer w={f->request,0,8192,1};put(&w,"{\"action\":");string(&w,s->action);
    put(&w,",\"epoch\":");number(&w,epoch);put(&w,",\"command_id\":");string(&w,command);
    put(&w,",\"script_id\":");string(&w,f->script_id);put(&w,",\"wake_source\":");string(&w,f->wake_source);
    if(f->wake_text){put(&w,",\"wake_text\":");string(&w,f->wake_text);}
    if(s->target){put(&w,",\"target\":");string(&w,s->target);}
    put(&w,",\"params\":");
    if(local){put(&w,"{\"op\":");string(&w,s->peer_op);put(&w,",\"params\":");}
    int ok=bindings(f,s->params,s->param_count,scratch,&w,1);
    if(local)put(&w,"}");
    else{
        put(&w,",\"set\":");ok=ok&&bindings(f,s->set,s->set_count,scratch,&w,0);
        static const char *const keys[]={"mic","speaker","ring"};put(&w,",\"audio\":{");
        for(unsigned i=0;i<3;i++){if(i)put(&w,",");string(&w,keys[i]);put(&w,":");string(&w,(s->audio&(1u<<i))?"open":"close");}
        put(&w,"}");
    }
    put(&w,"}");f->request_length=w.length;
    if(!ok||!w.ok){stop(f,BR_FLOW_FAILED,"ref_missing");return finish_call(f,scratch);}
    br_caps_plan plan;memset(&plan,0,sizeof(plan));
    int rc=local?br_caps_system_request(caps,epoch,f->request,w.length,scratch):
        br_caps_open_operation(caps,epoch,f->request,w.length,scratch,&plan);
    if(rc==BR_CAPS_OK&&f->session_epoch&&(plan.oneway||plan.requested_audio))rc=BR_CAPS_INVALID;
    if(rc==BR_CAPS_OK&&f->silent){
        int allowed=!local&&plan.silent&&!plan.requested_audio;
        if(local)for(size_t i=0;i<f->silent_rule_count;i++)
            if(!strcmp(f->silent_rules[i].op,s->peer_op)&&f->silent_rules[i].kind==BR_SYSTEM_ACK)allowed=1;
        if(!allowed)rc=BR_CAPS_INVALID;
    }
    if(rc!=BR_CAPS_OK||(!local&&plan.session_end&&f->index+1<f->definition->count)){
        stop(f,BR_FLOW_FAILED,rc==BR_CAPS_OK?"flow_invalid":br_caps_error(rc));return finish_call(f,scratch);
    }
    unsigned used=f->used;
    if(!local&&!declarations(f,caps,&plan)){stop(f,BR_FLOW_FAILED,"flow_invalid");return finish_call(f,scratch);}
    uint64_t timeout=local?120000:plan.connection_op.length==plan.op.length&&plan.op.length&&
        !memcmp(plan.connection_op.data,plan.op.data,plan.op.length)?plan.connection_timeout_ms:plan.timeout_ms;
    uint64_t remaining=900000-(now-f->started_ms);if(timeout>remaining)timeout=remaining;
    /* Persist identity and declarations before the adapter may send bytes. */
    f->ticket=ticket;f->epoch=epoch;f->local=local;f->oneway=plan.oneway;
    memcpy(f->command,command,37);
    br_flow_record *r=f->records+f->index;r->epoch=epoch;memcpy(r->command,command,37);
    if(!checkpoint(f,BR_FLOW_PREPARE)){
        stop(f,BR_FLOW_FAILED,"flow_storage");return finish_call(f,scratch);
    }
    if(f->checkpoint)f->last_ticket=ticket;
    rc=f->io.start(f->io.context,ticket,epoch,local,f->request,w.length,now,timeout);
    if(!rc){
        if(!checkpoint(f,BR_FLOW_NOT_SENT)){
            stop(f,BR_FLOW_FAILED,"flow_storage");return finish_call(f,scratch);
        }
        memset(f->arena+used,0,f->used-used);f->used=used;memset(f->records+f->index,0,sizeof(br_flow_record));
        f->ticket=0;f->epoch=0;memset(f->command,0,sizeof(f->command));
        wipe_request(f);return finish_call(f,scratch);
    }
    f->ticket=f->last_ticket=ticket;f->epoch=epoch;f->step_ms=now;f->timeout_ms=timeout;
    f->local=local;f->oneway=plan.oneway;f->acked=f->cancelled=0;f->task[0]=0;
    memcpy(f->command,command,37);f->status=BR_FLOW_WAIT;
    if(rc!=1)stop(f,BR_FLOW_FAILED,"bridge_unreachable");
    return finish_call(f,scratch);
}
static const char *code(const br_json *d,unsigned n){
    static const char *const codes[]={"bridge_unreachable","token_invalid","bridge_connect_timeout","app_not_installed",
        "hook_unavailable","hook_version_mismatch","app_version_unsupported","bridge_version_mismatch","session_missing",
        "search_empty","op_timeout","script_disabled","unsupported","quota_exhausted"};
    for(unsigned i=0;i<14;i++)if(br_json_is(d,n,codes[i]))return codes[i];
    return NULL;
}
static int private_result(const br_json *d,unsigned row){
    if(br_json_kind(d,row)!=BR_J_OBJECT)return 0;
    unsigned end=br_json_next(d,row);
    for(unsigned i=row;i<end;i++)if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return 0;
    unsigned t=field(d,row,"text"),j=field(d,row,"json");
    return (!t||br_json_kind(d,t)==BR_J_STRING)&&(!j||br_json_kind(d,j)==BR_J_OBJECT);
}
static void terminal(br_flow *f,const br_json *d,unsigned row){
    if(!private_result(d,row)){stop(f,BR_FLOW_FAILED,"flow_invalid");return;}
    br_text raw=br_json_text(d,row);br_flow_record *r=f->records+f->index;
    if(!save(f,raw.data,raw.length,&r->result,&r->result_length)){stop(f,BR_FLOW_FAILED,"flow_invalid");return;}
    if(!checkpoint(f,BR_FLOW_SUCCESS)){stop(f,BR_FLOW_FAILED,"flow_storage");return;}
    f->index++;f->status=BR_FLOW_DRAINING;
    f->after_drain=f->index==f->definition->count?BR_FLOW_DONE:BR_FLOW_NEXT;
}
static int current(br_flow *f,uint64_t run,uint64_t ticket,uint64_t now){
    /* Identity before time: a stale callback cannot time out/cancel new work. */
    return f&&ticket&&ticket==f->ticket&&run==f->run&&clock_check(f,run,now)&&f->status==BR_FLOW_WAIT;
}
int br_flow_operation(br_flow *f,uint64_t run,uint64_t ticket,uint64_t now,const br_operation *op,int event,br_json *scratch){
    if(!scratch||!f||!op||f->local||(op->epoch&&op->epoch!=f->epoch)||
       (op->epoch&&strcmp(op->command_id,f->command))||!current(f,run,ticket,now))return finish_call(f,scratch);
    if(event==BR_OP_CLOSED||event==BR_OP_TIMEOUT){
        /* operation abort wipes its identity, so only the owner's current
         * step ticket authorizes this closure notification. */
        if(!(f->oneway&&f->acked))stop(f,BR_FLOW_FAILED,event==BR_OP_TIMEOUT?"op_timeout":"bridge_unreachable");
    }else if(op->epoch==f->epoch&&!strcmp(op->command_id,f->command)&&event==op->last_event){
        if(event==BR_OP_ACK&&op->acked){memcpy(f->task,op->task_id,sizeof(f->task));f->acked=1;}
        else if(event==BR_OP_READY&&op->completed&&!f->oneway){
            if(f->session_epoch&&op->confirmed_audio)stop(f,BR_FLOW_FAILED,"unsupported");
            else terminal(f,&op->response,1);
        }
        else if(event==BR_OP_ERROR&&op->error)stop(f,BR_FLOW_FAILED,op->error);
    }
    return finish_call(f,scratch);
}
int br_flow_system(br_flow *f,uint64_t run,uint64_t ticket,uint64_t now,const br_system *s,br_json *scratch){
    if(!scratch||!f||!f->local||!s||s->ticket!=ticket||s->epoch!=f->epoch||
       !current(f,run,ticket,now))return finish_call(f,scratch);
    if(s->status==BR_SYSTEM_DONE){
        if(br_json_parse(scratch,"{}",2)==1)terminal(f,scratch,1);else stop(f,BR_FLOW_FAILED,"flow_invalid");
    }else if(s->status==BR_SYSTEM_FAILED)stop(f,BR_FLOW_FAILED,s->error?s->error:"unsupported");
    else if(s->status==BR_SYSTEM_CANCELLED)stop(f,BR_FLOW_CANCELLED,"flow_cancelled");
    return finish_call(f,scratch);
}
int br_flow_task(br_flow *f,uint64_t run,uint64_t ticket,uint64_t now,const br_json *d,unsigned row,
                 br_flow_receipt receipt,void *context,br_json *scratch){
    if(!scratch||!f||run!=f->run||ticket!=f->ticket||f->local||!f->oneway||!f->acked||!d||!receipt)return finish_call(f,scratch);
    uint64_t epoch;
    if(!br_json_is(d,field(d,row,"kind"),"result")||!br_json_u64(d,field(d,row,"epoch"),&epoch)||epoch!=f->epoch||
       !br_json_is(d,field(d,row,"command_id"),f->command)||!br_json_is(d,field(d,row,"task_id"),f->task))return finish_call(f,scratch);
    if(!current(f,run,ticket,now))return finish_call(f,scratch);
    if(!private_result(d,row)){stop(f,BR_FLOW_FAILED,"flow_invalid");return finish_call(f,scratch);}
    int rc=receipt(context,d,row);if(rc==0)return finish_call(f,scratch);
    unsigned c=field(d,row,"code");const char *error=c?code(d,c):NULL;
    if(rc!=1||(c&&!error))stop(f,BR_FLOW_FAILED,"flow_invalid");
    else if(error)stop(f,BR_FLOW_FAILED,error);
    else terminal(f,d,row);
    return finish_call(f,scratch);
}
int br_flow_output(br_flow *f,br_json *scratch,void *out,size_t cap,size_t *length){
    if(!f||!scratch||!out||!length||f->status!=BR_FLOW_DONE)return -1;
    br_flow_results view;result_view(f,&view);
    int rc=br_flow_saved_output(&view,scratch,out,cap,length);wipe_request(f);return rc;
}
int br_flow_saved_output(const br_flow_results *f,br_json *scratch,void *out,size_t cap,size_t *length){
    int rc=-1;
    if(!scratch)return -1;
    if(!f||!out||!length||!f->arena||f->used>BR_FLOW_ARENA||
       !definition_valid(f->definition,f->inputs,f->input_count,scratch)||f->count!=f->definition->count)goto done;
    unsigned offset=0;
    for(unsigned i=0;i<f->count;i++){
        const br_flow_record *r=f->records[i];
        if(!r)goto done;
        if(r->declarations_length){
            if(r->declarations!=offset||r->declarations_length>f->used-offset||
               br_json_parse(scratch,f->arena+offset,r->declarations_length)!=1||br_json_kind(scratch,1)!=BR_J_ARRAY)goto done;
            unsigned count=0;
            for(unsigned t=br_json_child(scratch,1);t&&t<br_json_next(scratch,1);t=br_json_next(scratch,t)){
                br_text text=br_json_text(scratch,t);
                if(++count>16||br_json_kind(scratch,t)!=BR_J_STRING||!text.length||text.length>32)goto done;
                for(size_t k=0;k<text.length;k++){
                    unsigned c=text.data[k];
                    if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'||(k&&c>='0'&&c<='9')))goto done;
                }
                for(unsigned k=br_json_child(scratch,1);k<t;k=br_json_next(scratch,k))if(br_json_equal(scratch,k,scratch,t))goto done;
            }
            offset+=r->declarations_length;
        }else if(r->declarations)goto done;
        if(r->result!=offset||!r->result_length||r->result_length>f->used-offset||
           br_json_parse(scratch,f->arena+offset,r->result_length)!=1||!private_result(scratch,1)||field(scratch,1,"code"))goto done;
        offset+=r->result_length;
    }
    if(offset!=f->used)goto done;
    /* Recheck every consumed reference against its original preceding record,
     * not only the selected output. No current caps can grant history fields. */
    br_flow_results prefix=*f;
    for(unsigned i=0;i<f->count;i++){
        prefix.count=i;const br_flow_step *s=f->definition->steps+i;
        const br_flow_binding *lists[]={s->params,s->set};const size_t counts[]={s->param_count,s->set_count};
        for(unsigned k=0;k<2;k++)for(size_t n=0;n<counts[k];n++){
            writer measure={NULL,0,(size_t)-1,1};if(!resolve(&prefix,&lists[k][n].value,scratch,&measure,k==0))goto done;
        }
    }
    br_flow_value fallback={BR_FLOW_TEXT,NULL,f->definition->steps[f->count-1].id,NULL};
    const br_flow_value *v=f->definition->output?f->definition->output:&fallback;
    writer measure={NULL,0,(size_t)-1,1};
    if(!resolve(f,v,scratch,&measure,0)){rc=f->definition->output?-1:0;goto done;}
    if(measure.length>8192||measure.length>cap)goto done;
    writer actual={out,0,cap,1};
    if(!resolve(f,v,scratch,&actual,0))goto done;
    *length=actual.length;rc=1;
done:
    memset(scratch,0,sizeof(*scratch));return rc;
}
int br_flow_poll(br_flow *f,uint64_t run,uint64_t now,int preempt,br_json *scratch){
    if(!scratch||!clock_check(f,run,now))return -1;
    if(preempt!=0&&preempt!=1)return finish_call(f,scratch);
    if(preempt)stop(f,BR_FLOW_CANCELLED,"flow_cancelled");
    if(f->status==BR_FLOW_DRAINING&&f->io.reaped(f->io.context,f->ticket,f->now_ms)==1){
        if(f->after_drain==BR_FLOW_DONE&&f->definition->output){
            writer measure={NULL,0,(size_t)-1,1};
            br_flow_results view;result_view(f,&view);
            if(!resolve(&view,f->definition->output,scratch,&measure,0)){
                f->after_drain=BR_FLOW_FAILED;f->error="ref_missing";
            }
        }
        if((f->after_drain==BR_FLOW_NEXT||f->after_drain==BR_FLOW_DONE)&&!checkpoint(f,BR_FLOW_REAPED)){
            f->after_drain=BR_FLOW_FAILED;f->error="flow_storage";
        }
        cleanup(f);
    }
    return finish_call(f,scratch);
}
