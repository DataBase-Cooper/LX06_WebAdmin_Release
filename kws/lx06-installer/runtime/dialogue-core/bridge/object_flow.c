/* LX06_TEST_FIXTURE_ONLY: original typed-object consumer, no production IO. */
#include "object_flow.h"
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
#include "selected-script-profile.h"
#endif
#include <stdio.h>
#include <string.h>
const char br_object_flow_fixture_guard[]="LX06_TEST_FIXTURE_ONLY";

int br_object_scalar(const struct eo_view *v,int n,char *out,size_t cap,size_t *length){
    if(!v||n<0||(unsigned)n>=v->count||!out||!length)return BR_OBJECT_INVALID;
    const struct eo_node *node=v->nodes+n;
    if(node->kind==EO_STRING){
        const unsigned char *s=(const unsigned char *)eo_str(v,n);
        size_t need=2;
        for(size_t i=0;s[i];i++)need+=s[i]<32?6:s[i]=='"'||s[i]=='\\'?2:1;
        if(need>=cap)return BR_OBJECT_INVALID;
        static const char hex[]="0123456789abcdef";
        size_t used=0;out[used++]='"';
        for(size_t i=0;s[i];i++){
            unsigned c=s[i];
            if(c<32){memcpy(out+used,"\\u00",4);used+=4;out[used++]=hex[c>>4];out[used++]=hex[c&15];}
            else {if(c=='"'||c=='\\')out[used++]='\\';out[used++]=(char)c;}
        }
        out[used++]='"';out[used]=0;*length=used;return BR_OBJECT_OK;
    }
    char number[32];const char *s=NULL;
    if(node->kind==EO_INT){(void)snprintf(number,sizeof(number),"%lld",(long long)eo_number(v,n));s=number;}
    else if(node->kind==EO_BOOL)s=eo_number(v,n)?"true":"false";
    else if(node->kind==EO_NULL)s="null";
    if(!s)return BR_OBJECT_UNSUPPORTED;
    size_t z=strlen(s);if(z>=cap)return BR_OBJECT_INVALID;
    memcpy(out,s,z+1);*length=z;return BR_OBJECT_OK;
}
/* Serialize A's already decoded complete row objects into the existing
 * bounded B literal carrier. No second parser, recursive values or slicing. */
static int object_rows(const struct eo_view *v,int n,char *out,size_t cap,size_t *length){
    if(v->nodes[n].kind!=EO_ARRAY||v->nodes[n].child<0)return BR_OBJECT_INVALID;
    size_t used=0,z=0;if(cap>8193)cap=8193;
#define ROW_BYTE(c) do{if(used+1>=cap)return BR_OBJECT_INVALID;out[used++]=(c);}while(0)
    ROW_BYTE('[');int row_count=0;
    for(int row=v->nodes[n].child;row>=0;row=v->nodes[row].next){
        if(v->nodes[row].kind!=EO_OBJECT)return BR_OBJECT_INVALID;
        if(row_count++)ROW_BYTE(',');
        ROW_BYTE('{');int fields=0;
        for(int k=v->nodes[row].child;k>=0;){int val=v->nodes[k].next;
            if(v->nodes[k].kind!=EO_STRING||!*eo_str(v,k)||
               (v->nodes[val].kind!=EO_STRING&&v->nodes[val].kind!=EO_INT&&v->nodes[val].kind!=EO_BOOL))return BR_OBJECT_INVALID;
            if(fields++)ROW_BYTE(',');
            if(br_object_scalar(v,k,out+used,cap-used,&z)!=1)return BR_OBJECT_INVALID;
            used+=z;ROW_BYTE(':');
            if(br_object_scalar(v,val,out+used,cap-used,&z)!=1)return BR_OBJECT_INVALID;
            used+=z;k=v->nodes[val].next;
        }ROW_BYTE('}');
    }ROW_BYTE(']');out[used]=0;*length=used;return BR_OBJECT_OK;
#undef ROW_BYTE
}
static const char *copy(br_object_flow *o,const char *s){
    if(!s)return NULL;
    size_t n=strlen(s)+1;if(n>sizeof(o->arena)-o->used)return NULL;
    char *p=o->arena+o->used;memcpy(p,s,n);o->used+=(unsigned)n;return p;
}
static int value(br_object_flow *o,const struct eo_view *v,int n,br_flow_value *out){
    if(n<0)return BR_OBJECT_INVALID;
    if(eo_scalar(v,n)){
        size_t z=0;char *p=o->arena+o->used;
        int rc=br_object_scalar(v,n,p,sizeof(o->arena)-o->used,&z);
        if(rc!=1)return rc;
        o->used+=(unsigned)z+1;*out=(br_flow_value){BR_FLOW_LITERAL,p,NULL,NULL};return 1;
    }
    if(v->nodes[n].kind==EO_ARRAY){
        size_t z=0;char *p=o->arena+o->used;
        int rc=object_rows(v,n,p,sizeof(o->arena)-o->used,&z);if(rc!=1)return rc;
        o->used+=(unsigned)z+1;*out=(br_flow_value){BR_FLOW_LITERAL,p,NULL,NULL};return BR_OBJECT_OK;
    }
    const char *ref=eo_str(v,eo_get(v,n,"$ref"));
    if(!ref)return BR_OBJECT_INVALID;
    if(!strncmp(ref,"input.",6)){
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
        if(o->inputs_verified)for(size_t i=0;i<o->input_count;i++)if(!strcmp(o->inputs[i].key,ref+6)){
            const char *key=copy(o,ref+6);if(!key)return BR_OBJECT_INVALID;
            *out=(br_flow_value){BR_FLOW_INPUT,NULL,NULL,key};return 1;
        }
#endif
        return BR_OBJECT_UNVERIFIED;
    }
    const char *dot=strchr(ref+6,'.');if(!dot)return BR_OBJECT_INVALID;
    if(strcmp(dot,".text"))return BR_OBJECT_UNVERIFIED;
    const char *p=copy(o,ref+6);if(!p)return BR_OBJECT_INVALID;
    ((char *)p)[dot-ref-6]=0;
    *out=(br_flow_value){BR_FLOW_TEXT,NULL,p,NULL};return 1;
}
static int bindings(br_object_flow *o,const struct eo_view *v,int n,br_flow_binding *b,size_t *count){
    *count=0;if(n<0)return 1;
    for(int k=v->nodes[n].child;k>=0;){
        if(*count==16)return BR_OBJECT_UNSUPPORTED;
        int a=v->nodes[k].next;br_flow_binding *entry=b+(*count)++;
        entry->key=copy(o,eo_str(v,k));if(!entry->key)return BR_OBJECT_INVALID;
        int rc=value(o,v,a,&entry->value);if(rc!=1)return rc;
        k=v->nodes[a].next;
    }
    return 1;
}
typedef struct { br_object_flow *out;const char *name; } request;
static int consume(void *p,const struct eo_view *v,const struct eo_origin *origin){
    request *r=p;br_object_flow *o=r->out;
    if(v->flows<0)return BR_OBJECT_INVALID;
    int flow=-1;
    for(int f=v->nodes[v->flows].child;f>=0;f=v->nodes[f].next)
        if(eo_eq(v,eo_get(v,f,"name"),r->name)){flow=f;break;}
    if(flow<0)return BR_OBJECT_INVALID;
    int inputs=eo_get(v,flow,"inputs");
    if(inputs>=0&&v->nodes[inputs].child>=0
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
       &&!o->inputs_verified
#endif
    )return BR_OBJECT_UNVERIFIED;
    int list=eo_get(v,flow,"steps");
    for(int n=v->nodes[list].child;n>=0;n=v->nodes[n].next){
        unsigned i=(unsigned)o->definition.count;
        if(i==BR_FLOW_STEPS)return BR_OBJECT_INVALID;
        const char *unsupported[]={"precondition","done","retry","restart","fallback","on_probe_unknown","timeout_ms"};
        for(unsigned k=0;k<sizeof(unsupported)/sizeof(*unsupported);k++)
            if(eo_get(v,n,unsupported[k])>=0)return BR_OBJECT_UNSUPPORTED;
        int policy=eo_get(v,n,"policy");
        if(policy>=0&&!eo_eq(v,policy,"always-re-run"))return BR_OBJECT_UNSUPPORTED;
        br_flow_step *s=o->steps+i;
        s->id=copy(o,eo_str(v,eo_get(v,n,"id")));
        s->action=copy(o,eo_str(v,eo_get(v,n,"action")));
        int target=eo_get(v,n,"target");s->target=target<0?NULL:copy(o,eo_str(v,target));
        if(!s->id||!s->action||(target>=0&&!s->target))return BR_OBJECT_INVALID;
        /* A's call shape has no peer_op projection. Never guess system rules. */
        if(!strcmp(s->action,"system.op"))return BR_OBJECT_UNVERIFIED;
        int rc=bindings(o,v,eo_get(v,n,"params"),o->params[i],&s->param_count);if(rc!=1)return rc;
        rc=bindings(o,v,eo_get(v,n,"set"),o->set[i],&s->set_count);if(rc!=1)return rc;
        s->params=o->params[i];s->set=o->set[i];
        int audio=eo_get(v,n,"audio");const char *keys[]={"mic","speaker","ring"};
        for(unsigned k=0;k<3;k++)if(eo_eq(v,eo_get(v,audio,keys[k]),"open"))s->audio|=1u<<k;
        o->definition.count++;
    }
    int output=eo_get(v,flow,"output");
    if(output>=0){
        int rc=value(o,v,output,&o->output);if(rc!=1)return rc;
        /* Original stepper supports referenced outputs only. */
        if(o->output.source==BR_FLOW_LITERAL)return BR_OBJECT_UNSUPPORTED;
        o->definition.output=&o->output;
    }
    o->definition.steps=o->steps;o->origin=*origin;return 1;
}
int br_object_flow_copy(br_object_flow *o,struct eo_owner *owner,const struct eo_origin *origin,const char *name){
    if(!o||!owner||!origin||!name)return BR_OBJECT_INVALID;
    memset(o,0,sizeof(*o));request r={o,name};
    int rc=eo_consume(owner,origin,consume,&r);
    if(rc!=1)memset(o,0,sizeof(*o));
    return rc;
}
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
static int consume_prepared(void *p,const struct eo_view *v,const struct eo_origin *origin,
    const struct eo_view *inputs,const char *digest){
    request *r=p;br_object_flow *o=r->out;
    if(!inputs||!digest)return BR_OBJECT_UNVERIFIED;
    int rc=bindings(o,inputs,0,o->inputs,&o->input_count);if(rc!=1)return rc;
    o->inputs_verified=1;memcpy(o->inputs_sha256,digest,65);
    return consume(p,v,origin);
}
int br_object_flow_copy_prepared(br_object_flow *o,struct eo_owner *source,const struct eo_origin *origin,const char *name){
    if(!o||!source||!origin||!name)return BR_OBJECT_INVALID;
    memset(o,0,sizeof(*o));request r={o,name};
    int rc=eo_inputs_consume(source,origin,name,consume_prepared,&r);
    if(rc!=1)memset(o,0,sizeof(*o));return rc;
}
static int run_identity(const br_object_flow_run *r,uint64_t run){
    return r&&!r->busy&&r->held&&r->source&&r->adapter&&r->log&&r->owner&&
        r->adapter->run==run&&r->log->binding.run==run&&r->source->in_flight&&
        eo_same(&r->origin,&r->source->origin);
}
static int start(br_object_flow_run *r,br_object_flow *o,struct eo_owner *source,
    const struct eo_origin *origin,const char *name,br_flow_session *a,br_flow_session_log *j,
    br_flow_owner_store *owner,uint64_t now,br_object_source_current current,void *context,
    br_flow_owner_handoff_current previous,void *previous_context,int prepared,struct selected_script_profile *selected){
    if(!r||r->held||r->busy||!o||!source||!origin||!a||!j||!owner||!current||!previous||
       !name||!a->session||a->busy||a->bound||j->readonly||j->phase||j->blocked||
       (j->binding.source!=BR_LOG_LOCAL&&j->binding.source!=BR_LOG_OFFICIAL)||!a->session->live||a->session->owned||
       current(context,origin)!=1)return BR_OBJECT_UNVERIFIED;
    int rc=prepared?br_object_flow_copy_prepared(o,source,origin,name):br_object_flow_copy(o,source,origin,name);if(rc!=1)return rc;
    const br_flow_session_log_binding *b=&j->binding;
    if(b->count!=o->definition.count||strcmp(b->flow_sha256,origin->flow)||
       strcmp(b->revision,origin->settings)||owner->workspace!=a->wire||
       (prepared&&strcmp(b->inputs_sha256,o->inputs_sha256)))return BR_OBJECT_INVALID;
    for(unsigned i=0;i<o->definition.count;i++)if(o->steps[i].audio)return BR_OBJECT_UNSUPPORTED;
    if(current(context,origin)!=1)return BR_OBJECT_UNVERIFIED;
    if(selected) {
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
        if(!prepared||selected_script_flow_handoff(selected,source,origin,b->session_run,b->epoch,b->run,r)!=1)
            return BR_OBJECT_UNVERIFIED;
#else
        return BR_OBJECT_UNVERIFIED;
#endif
    } else if(eo_hold(source,origin))return BR_OBJECT_UNVERIFIED;
    *r=(br_object_flow_run){.object=o,.source=source,.origin=*origin,.adapter=a,.log=j,
        .owner=owner,.current=current,.context=context,.held=1,.busy=1};
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    r->selected=selected;
#endif
    static const char *const sources[]={"","official","local"};
    rc=br_flow_begin(a->flow,b->run,now,&o->definition,o->inputs,o->input_count,b->script,sources[b->source],
        "",&a->session->operation.discovery->scratch);
    if(rc==1)rc=br_flow_session_bind(a,b->run,b->session_run,b->epoch,b->generation);
    if(rc==1)rc=br_flow_session_log_attach(j,a);
    if(rc==1)rc=br_flow_owner_store_bind(owner,j,previous,previous_context);
    r->busy=0;return rc==1?1:BR_OBJECT_UNVERIFIED;
}
int br_object_flow_start(br_object_flow_run *r,br_object_flow *o,struct eo_owner *s,
    const struct eo_origin *g,const char *n,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *w,
    uint64_t t,br_object_source_current c,void *x,br_flow_owner_handoff_current p,void *y){
    return start(r,o,s,g,n,a,j,w,t,c,x,p,y,0,NULL);
}
int br_object_flow_start_prepared(br_object_flow_run *r,br_object_flow *o,struct eo_owner *s,
    const struct eo_origin *g,const char *n,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *w,
    uint64_t t,br_object_source_current c,void *x,br_flow_owner_handoff_current p,void *y){
    return start(r,o,s,g,n,a,j,w,t,c,x,p,y,1,NULL);
}
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
int br_object_flow_start_prepared_selected(br_object_flow_run *r,br_object_flow *o,struct eo_owner *s,
    const struct eo_origin *g,const char *n,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *w,
    uint64_t t,br_object_source_current c,void *x,br_flow_owner_handoff_current p,void *y,
    struct selected_script_profile *selected){
    if(!selected)return BR_OBJECT_UNVERIFIED;
    return start(r,o,s,g,n,a,j,w,t,c,x,p,y,1,selected);
}
#endif
int br_object_flow_dispatch(br_object_flow_run *r,uint64_t run,uint64_t ticket,uint64_t now,const char *command){
    if(!run_identity(r,run)||r->current(r->context,&r->origin)!=1)return BR_OBJECT_UNVERIFIED;
    r->busy=1;int rc=br_flow_session_dispatch(r->adapter,run,ticket,now,command);r->busy=0;return rc;
}
int br_object_flow_event(br_object_flow_run *r,uint64_t run,uint64_t ticket,uint64_t now,int event){
    if(!run_identity(r,run))return BR_OBJECT_UNVERIFIED;
    r->busy=1;int rc=br_flow_session_event(r->adapter,run,ticket,now,event);r->busy=0;return rc;
}
int br_object_flow_poll(br_object_flow_run *r,uint64_t run,uint64_t now,int preempt){
    if(!run_identity(r,run))return BR_OBJECT_UNVERIFIED;
    r->busy=1;int rc=br_flow_session_poll(r->adapter,run,now,preempt);r->busy=0;return rc;
}
const char *br_object_flow_outcome(const br_object_flow_run *r){
    if(!r||!r->adapter)return "unstarted";
    const br_flow *f=r->adapter->flow;
    if(f->status==BR_FLOW_CANCELLED||f->after_drain==BR_FLOW_CANCELLED)return "cancelled";
    if(f->deadline_expired||(f->error&&!strcmp(f->error,"op_timeout")))return "timeout";
    if(f->error&&!strcmp(f->error,"bridge_unreachable"))return "UNKNOWN";
    if(f->status==BR_FLOW_FAILED||f->after_drain==BR_FLOW_FAILED)return "failed";
    if(f->status==BR_FLOW_DONE)return "success";
    if(r->held&&!r->adapter->session->live)return "UNKNOWN";
    return r->held?"running":"unstarted";
}
typedef struct {br_object_flow_run *run;br_flow_session_log_closed closed;void *context;} source_release;
int br_object_flow_result(br_object_flow_run *r,br_inbox *inbox,uint64_t wall,int trusted,
    br_result *identity,br_result_progress *progress){
    if(!r||!r->adapter||!run_identity(r,r->adapter->run)||!inbox||!identity||!progress||
       r->result_handed_off||r->adapter->flow->status!=BR_FLOW_DONE||
       r->log->phase!=BR_FLOW_REAPED||r->log->index+1!=r->log->binding.count)return BR_INVALID;
    r->busy=1;br_flow_session *a=r->adapter;br_flow_session_log_binding *b=&r->log->binding;
    br_json *scratch=&a->session->operation.discovery->scratch;
    br_result result={.origin=1,.epoch=b->epoch};size_t n=0;int rc=BR_INVALID;
    strcpy(result.command_id,r->log->steps[0].command);strcpy(result.local.script,b->script);
    strcpy(result.local.revision,b->revision);memcpy(result.local.task,b->flow_sha256,32);
    result.local.serial=b->run;
    int output=br_flow_output(a->flow,scratch,a->wire,BR_FLOW_SESSION_WIRE,&n);
    if(output<0)goto done;
    if(output==1){
        if(br_json_parse(scratch,a->wire,n)!=1)goto done;
        br_text t=br_json_text(scratch,1);
        if(t.length>=sizeof result.text||memchr(t.data,0,t.length))goto done;
        memcpy(result.text,t.data,t.length);
    }
    rc=br_inbox_sync_receive(inbox,&result,wall,trusted,0,86400);
    if(rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT||rc==BR_DUPLICATE){
        *identity=result;memset(identity->text,0,sizeof(identity->text));
        r->result_handed_off=1;
        rc=br_inbox_progress_read(inbox,identity,progress);
    }
done:
    memset(&result,0,sizeof result);memset(scratch,0,sizeof(*scratch));
    memset(a->wire,0,BR_FLOW_SESSION_WIRE);r->busy=0;return rc;
}
static int release_saved(void *p,const struct eo_view *v,const struct eo_origin *origin){
    source_release *x=p;br_object_flow_run *r=x->run;br_flow_session_log *j=r->log;
    return v&&eo_same(origin,&r->origin)&&j->phase==BR_FLOW_SESSION_LOG_RETIRED&&
        j->index+1==j->binding.count&&x->closed(x->context,&j->binding,j->steps,j->binding.count)==1;
}
int br_object_flow_retire(br_object_flow_run *r,br_flow_session_log_closed closed,void *context){
    if(!r||!r->adapter||!run_identity(r,r->adapter->run)||!closed||
       !r->result_handed_off||r->adapter->flow->status!=BR_FLOW_DONE)return BR_OBJECT_UNVERIFIED;
    r->busy=1;
    int rc=br_flow_owner_store_retire_closed(r->owner,r->log,closed,context);
    if(rc==1){
        source_release proof={r,closed,context};
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
        if(r->selected)rc=selected_script_flow_release(r->selected,r,&r->origin,release_saved,&proof)?-1:1;
        else
#endif
        rc=eo_release(r->source,&r->origin,release_saved,&proof)?-1:1;
    }
    if(rc==1){r->held=0;
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
        r->selected=NULL;
#endif
        (void)eo_inputs_discard(r->source);}
    r->busy=0;return rc;
}
static int new_intent(br_object_flow_run *r,br_object_flow *o,struct eo_owner *source,
    const struct eo_origin *origin,const char *name,br_flow_session *a,br_flow_session_log *j,
    br_flow_owner_store *owner,const br_flow_session_log_binding *b,uint64_t now,
    br_object_source_current current,void *context,br_flow_owner_handoff_current previous,void *previous_context,
    br_flow_session_log_closed closed,void *closed_context,int prepared){
    if(!r||r->held||r->busy||!o||!source||!origin||!a||!a->session||a->bound||a->busy||
       !j||j->blocked||j->phase!=BR_FLOW_SESSION_LOG_RETIRED||!owner||!b||!current||!previous||!closed||
       (b->source!=BR_LOG_LOCAL&&b->source!=BR_LOG_OFFICIAL)||
       b->run<=j->binding.run||b->session_run<=j->binding.session_run||b->epoch<=j->binding.epoch||
       b->generation<=j->binding.generation||current(context,origin)!=1)return BR_OBJECT_UNVERIFIED;
    int rc=prepared?br_object_flow_copy_prepared(o,source,origin,name):br_object_flow_copy(o,source,origin,name);if(rc!=1)return rc;
    if(b->count!=o->definition.count||strcmp(b->flow_sha256,origin->flow)||strcmp(b->revision,origin->settings)||
       owner->workspace!=a->wire||(prepared&&strcmp(b->inputs_sha256,o->inputs_sha256)))return BR_OBJECT_INVALID;
    for(unsigned i=0;i<o->definition.count;i++)if(o->steps[i].audio)return BR_OBJECT_UNSUPPORTED;
    if(current(context,origin)!=1||eo_hold(source,origin))return BR_OBJECT_UNVERIFIED;
    *r=(br_object_flow_run){.object=o,.source=source,.origin=*origin,.adapter=a,.log=j,.owner=owner,
        .current=current,.context=context,.held=1,.busy=1};
    static const char *const sources[]={"","official","local"};
    rc=br_flow_begin(a->flow,b->run,now,&o->definition,o->inputs,o->input_count,b->script,sources[b->source],"",
        &a->session->operation.discovery->scratch);
    if(rc==1)rc=br_flow_session_bind(a,b->run,b->session_run,b->epoch,b->generation);
    if(rc==1)rc=br_flow_owner_store_rebind_closed(owner,j,b,a,previous,previous_context,closed,closed_context);
    r->busy=0;return rc==1?1:BR_OBJECT_UNVERIFIED;
}
int br_object_flow_new_intent(br_object_flow_run *r,br_object_flow *o,struct eo_owner *s,
    const struct eo_origin *g,const char *n,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *w,
    const br_flow_session_log_binding *b,uint64_t t,br_object_source_current c,void *x,
    br_flow_owner_handoff_current p,void *y,br_flow_session_log_closed z,void *q){
    return new_intent(r,o,s,g,n,a,j,w,b,t,c,x,p,y,z,q,0);
}
int br_object_flow_new_intent_prepared(br_object_flow_run *r,br_object_flow *o,struct eo_owner *s,
    const struct eo_origin *g,const char *n,br_flow_session *a,br_flow_session_log *j,br_flow_owner_store *w,
    const br_flow_session_log_binding *b,uint64_t t,br_object_source_current c,void *x,
    br_flow_owner_handoff_current p,void *y,br_flow_session_log_closed z,void *q){
    return new_intent(r,o,s,g,n,a,j,w,b,t,c,x,p,y,z,q,1);
}
#endif
