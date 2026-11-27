/* LX06_TEST_FIXTURE_ONLY: consumes A objects, never reparses settings. */
#include "object_schedule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
const char br_object_schedule_fixture_guard[]="LX06_TEST_FIXTURE_ONLY";
typedef struct { br_object_schedule *out;const uint64_t *seeds; } copy_request;
static const char *text(br_object_schedule *o,const char *s){
    size_t n=strlen(s)+1;if(n>sizeof(o->arena)-o->used)return NULL;
    char *p=o->arena+o->used;memcpy(p,s,n);o->used+=(unsigned)n;return p;
}
static const char *scalar(br_object_schedule *o,const struct eo_view *v,int n){
    size_t z=0;char *p=o->arena+o->used;
    if(br_object_scalar(v,n,p,sizeof(o->arena)-o->used,&z)!=1)return NULL;
    o->used+=(unsigned)z+1;return p;
}
/* Typed scalar params are emitted directly into the same bounded run arena.
 * No input/settings JSON is parsed; unsupported structured values refuse. */
static const char *params(br_object_schedule *o,const struct eo_view *v,int n){
    unsigned start=o->used;
    if(sizeof(o->arena)-o->used<3)return NULL;
    o->arena[o->used++]='{';int first=1;
    if(n>=0)for(int k=v->nodes[n].child;k>=0;){
        if(!first){if(o->used>=sizeof(o->arena))return NULL;o->arena[o->used++]=',';}
        size_t z=0;
        if(br_object_scalar(v,k,o->arena+o->used,sizeof(o->arena)-o->used,&z)!=1)return NULL;
        o->used+=(unsigned)z;
        if(o->used>=sizeof(o->arena))return NULL;
        o->arena[o->used++]=':';int a=v->nodes[k].next;
        if(br_object_scalar(v,a,o->arena+o->used,sizeof(o->arena)-o->used,&z)!=1)return NULL;
        o->used+=(unsigned)z;k=v->nodes[a].next;first=0;
    }
    if(sizeof(o->arena)-o->used<2)return NULL;
    o->arena[o->used++]='}';o->arena[o->used++]=0;return o->arena+start;
}
static int consume(void *p,const struct eo_view *v,const struct eo_origin *origin){
    copy_request *r=p;br_object_schedule *o=r->out;
    if(v->schedules<0)return BR_OBJECT_INVALID;
    struct eo_task t;
    for(unsigned i=0;!eo_task_at(v,i,&t);i++){
        br_schedule_task *b=o->tasks+i;
        memcpy(b->id,t.id,sizeof(b->id));b->enabled=t.enabled;b->audible=t.audible;
        b->grace_s=t.grace_s;b->announce_ttl_s=t.announce_ttl_s;b->seed=r->seeds[i];
        if(!b->seed)return BR_OBJECT_INVALID;
        b->calendar.kind=(enum br_calendar_kind)t.calendar;
        b->calendar.first_minute=t.first_minute;b->calendar.last_minute=t.last_minute;
        b->calendar.interval_days=t.interval_days;
        if(t.calendar==BR_CAL_EVERY_N&&br_calendar_day(t.year,t.month,t.day,&b->calendar.anchor_day)!=1)return BR_OBJECT_INVALID;
        if(t.calendar==BR_CAL_WEEKLY)b->calendar.weekdays=t.days;
        if(t.calendar==BR_CAL_MONTHLY)b->calendar.monthdays=t.days;
        if(!br_calendar_valid(&b->calendar))return BR_OBJECT_INVALID;
        int f=eo_get(v,t.run_node,"flow");
        if(f>=0){o->run[i].flow=text(o,eo_str(v,f));if(!o->run[i].flow)return BR_OBJECT_INVALID;
            o->run[i].inputs=eo_get(v,t.run_node,"inputs")>=0;}
        else {
            o->run[i].action=scalar(o,v,eo_get(v,t.run_node,"action"));
            int target=eo_get(v,t.run_node,"target");
            o->run[i].target=target>=0?scalar(o,v,target):NULL;
            o->run[i].params=params(o,v,eo_get(v,t.run_node,"params"));
            if(!o->run[i].action||(target>=0&&!o->run[i].target)||!o->run[i].params)return BR_OBJECT_UNSUPPORTED;
        }
        o->count++;
    }
    o->origin=*origin;
    unsigned char source[200],digest[32];
    memcpy(source,origin->script,64);memcpy(source+64,origin->flow,64);memcpy(source+128,origin->settings,64);
    for(unsigned i=0;i<8;i++)source[192+i]=(unsigned char)(origin->revision>>(8*i));
    if(!SHA256(source,sizeof(source),digest))return BR_OBJECT_INVALID;
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){o->revision[i*2]=hex[digest[i]>>4];o->revision[i*2+1]=hex[digest[i]&15];}
    memset(source,0,sizeof(source));memset(digest,0,sizeof(digest));return 1;
}
int br_object_schedule_copy(br_object_schedule *o,struct eo_owner *owner,const struct eo_origin *origin,const uint64_t seeds[32]){
    if(!o||!owner||!origin||!seeds)return BR_OBJECT_INVALID;
    memset(o,0,sizeof(*o));copy_request r={o,seeds};int rc=eo_consume(owner,origin,consume,&r);
    if(rc!=1)memset(o,0,sizeof(*o));
    return rc;
}
typedef struct { br_object_schedule *o;br_schedule_runner *r;const struct eo_clock *clock;br_schedule_candidate *out; } poll_request;
static int bound_at(const br_object_schedule *o,const br_schedule_runner *r,size_t i){
    return o&&r&&i<r->count&&r->scripts[i]&&r->scripts[i]->tasks==o->tasks&&
        r->scripts[i]->count==o->count&&!strcmp(r->scripts[i]->revision,o->revision);
}
static int bound(const br_object_schedule *o,const br_schedule_runner *r){
    return r&&r->count==1&&bound_at(o,r,0);
}
static int poll_view(void *p,const struct eo_view *v,const struct eo_origin *origin){
    (void)v;poll_request *q=p;
    if(!eo_same(origin,&q->o->origin)||!bound(q->o,q->r))return BR_OBJECT_UNVERIFIED;
    return br_schedule_runner_poll(q->r,q->clock->trusted,q->clock->wall_ms,q->clock->mono_ms,q->clock->idle,q->out);
}
int br_object_schedule_poll(br_object_schedule *o,struct eo_owner *owner,br_schedule_runner *r,const struct eo_clock *clock,br_schedule_candidate *out){
    if(!o||!owner||!r||!clock||!out)return BR_OBJECT_INVALID;
    if(o->held)return 0;
    poll_request q={o,r,clock,out};
    int rc=eo_schedule_consume(owner,&o->origin,clock,poll_view,&q);
    if(rc<0){
        /* No candidate survives source/clock/activity refusal. Original poll
         * enforces journal busy/reentry guards before changing anything. */
        br_schedule_candidate ignored;
        (void)br_schedule_runner_poll(r,0,clock->wall_ms,clock->mono_ms,0,&ignored);
    }
    return rc;
}
static int no_claim(void *p,const struct eo_view *v,const struct eo_origin *origin){
    (void)p;(void)v;(void)origin;return 1;
}
static int start_resolved(br_object_schedule *o,struct eo_owner *owner,br_schedule_runner *r,
    const struct eo_clock *clock,br_schedule_candidate c,uint64_t run,uint64_t epoch,
    const char *uuid,char *open,size_t cap){
    if(o->run[c.slot].flow)return BR_OBJECT_UNVERIFIED;
    /* Preserve the full deployed filename (including dots) in OPEN/K6. This
     * JSON-safe alphabet excludes quotes, escapes and control characters. */
    const char *script=r->scripts[c.script]->script;
    for(const char *p=script;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||
        (*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return BR_OBJECT_UNSUPPORTED;
    if(strlen(uuid)!=36||!epoch)return BR_OBJECT_INVALID;
    for(unsigned i=0;i<36;i++)if(!((uuid[i]>='0'&&uuid[i]<='9')||(uuid[i]>='a'&&uuid[i]<='f')||uuid[i]=='-'))return BR_OBJECT_INVALID;
    int n=snprintf(open,cap,"{\"action\":%s,%s%s%s\"params\":%s,\"script_id\":\"%s\",\"epoch\":%llu,\"command_id\":\"%s\",\"wake_source\":\"scheduled\"}",
        o->run[c.slot].action,o->run[c.slot].target?"\"target\":":"",o->run[c.slot].target?o->run[c.slot].target:"",
        o->run[c.slot].target?",":"",o->run[c.slot].params,script,(unsigned long long)epoch,uuid);
    if(n<=0||(size_t)n>=cap||n>8192){memset(open,0,cap);return BR_OBJECT_INVALID;}
    br_discovery *d=r->op?r->op->discovery:NULL;br_caps_plan plan;
    int valid=d&&br_caps_open_operation(&d->caps,epoch,open,(size_t)n,&d->scratch,&plan)==BR_CAPS_OK;
    if(!valid||plan.oneway||(!o->tasks[c.slot].audible&&!plan.silent)){
        memset(open,0,cap);return BR_OBJECT_UNVERIFIED;
    }
    if(eo_hold(owner,&o->origin)){memset(open,0,cap);return BR_OBJECT_INVALID;}
    o->held=1;o->slot=c.slot;
    int rc=br_schedule_runner_start(r,run,c,epoch,uuid,clock->mono_ms,
        &(br_schedule_command){r->scripts[c.script]->revision,o->tasks[c.slot].id,open,(size_t)n});
    memset(open,0,cap);
    /* Even a refused/unsent attempt stays held for explicit owner resolution;
     * never invent a K6 terminal to unlock an ambiguous action. */
    if(r->run==run){o->serial=r->serial;o->epoch=r->epoch;memcpy(o->command,r->command,37);}
    else if(r->status==BR_RUN_IDLE&&r->run==0&&r->last_run<run){
        /* Original start did not claim/acquire anything (including busy).
         * This is an unsent local refusal, not a terminal/K6 receipt. */
        if(!eo_release(owner,&o->origin,no_claim,NULL))o->held=0;
    }
    return rc;
}
int br_object_schedule_start(br_object_schedule *o,struct eo_owner *owner,br_schedule_runner *r,
    const struct eo_clock *clock,uint64_t run,uint64_t epoch,const char *uuid,char *open,size_t cap){
    if(!o||!owner||!r||!clock||!uuid||!open||cap<8193||o->held||!bound(o,r)||
       !r->candidate||r->next.script||r->next.slot>=o->count)return BR_OBJECT_INVALID;
    br_schedule_candidate before=r->next,c={0,0};
    if(br_object_schedule_poll(o,owner,r,clock,&c)!=1||c.script!=before.script||c.slot!=before.slot)return BR_OBJECT_INVALID;
    return start_resolved(o,owner,r,clock,c,run,epoch,uuid,open,cap);
}
typedef struct { br_object_schedule *o;br_schedule_runner *r;br_inbox *inbox;br_result *scratch;unsigned script; } release_request;
static int release_view(void *p,const struct eo_view *v,const struct eo_origin *origin){
    (void)v;release_request *q=p;br_object_schedule *o=q->o;br_schedule_runner *r=q->r;
    if(!eo_same(origin,&o->origin)||!bound_at(o,r,q->script)||r->status!=BR_RUN_IDLE||
       r->scripts[q->script]->owned||!o->serial||o->slot>=o->count)return 0;
    const br_schedule_record *rec=r->scripts[q->script]->records+o->slot;
    if(rec->phase!=BR_SCHED_TERMINAL||rec->serial!=o->serial||rec->epoch!=o->epoch||strcmp(rec->command,o->command))return 0;
    br_result *id=q->scratch;memset(id,0,sizeof(*id));id->origin=1;id->epoch=o->epoch;
    memcpy(id->command_id,o->command,37);strcpy(id->local.script,r->scripts[q->script]->script);
    strcpy(id->local.revision,r->scripts[q->script]->revision);strcpy(id->local.task,o->tasks[o->slot].id);id->local.serial=o->serial;
    int rc=br_inbox_sync_receipt(q->inbox,id,(int)o->tasks[o->slot].audible);
    memset(id,0,sizeof(*id));return rc==1;
}
static int release_at(br_object_schedule *o,struct eo_owner *owner,br_schedule_runner *r,
    unsigned script,br_inbox *inbox,br_result *scratch){
    if(!o||!owner||!r||!inbox||!scratch)return BR_OBJECT_INVALID;
    memset(scratch,0,sizeof(*scratch));
    if(!o->held)return BR_OBJECT_INVALID;
    release_request q={o,r,inbox,scratch,script};
    int rc=eo_release(owner,&o->origin,release_view,&q);
    memset(scratch,0,sizeof(*scratch));
    if(!rc)o->held=0;
    return rc?BR_OBJECT_INVALID:BR_OBJECT_OK;
}
int br_object_schedule_release(br_object_schedule *o,struct eo_owner *owner,br_schedule_runner *r,br_inbox *inbox,br_result *scratch){
    if(!bound(o,r)){if(scratch)memset(scratch,0,sizeof(*scratch));return BR_OBJECT_INVALID;}
    return release_at(o,owner,r,0,inbox,scratch);
}
/* Retained-source binding for the existing all-device registry. Scheduling,
 * budgets, reconciliation, durable intent and recovery remain in schedule_run. */
static int registry_shape(const br_object_schedule_registry *g){
    if(!g||!g->ready||!g->runner||g->count>BR_SCHEDULE_TASKS||g->runner->count!=g->count)return 0;
    for(size_t i=0;i<g->count;i++){
        const br_object_schedule_binding *b=&g->entries[i].binding;
        if(!b->owner||!b->object||!b->schedule||g->runner->scripts[i]!=b->schedule||
           !bound_at(b->object,g->runner,i)||!eo_same(&b->object->origin,&g->entries[i].origin)||
           strcmp(b->schedule->script,g->entries[i].script)||
           strcmp(b->schedule->revision,g->entries[i].revision))return 0;
        for(size_t j=0;j<i;j++)if(b->owner==g->entries[j].binding.owner||
            b->object==g->entries[j].binding.object||b->schedule==g->entries[j].binding.schedule)return 0;
    }
    return 1;
}
static int registry_view(void *p,const struct eo_view *v,const struct eo_origin *origin){
    const struct eo_origin *expected=p;
    return v->schedules>=0&&eo_same(origin,expected)?1:BR_OBJECT_INVALID;
}
static int registry_current(br_object_schedule_registry *g){
    if(!registry_shape(g))return 0;
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
    if((g->scope||g->scope_current||g->scope_release)&&(!g->scope||!g->scope_current||
        !g->scope_release||g->scope_current(g->scope)!=1))return 0;
    for(size_t i=0;i<g->count;i++){
        const br_object_schedule_binding *b=&g->entries[i].binding;
        struct eo_saved_source *s=g->entries[i].source;
        if(!s||s->family!=2||s->owner!=b->owner||b->owner->schedule_source!=s||
           eo_saved_current(s,&g->entries[i].origin)!=1||
           eo_consume(b->owner,&g->entries[i].origin,registry_view,&g->entries[i].origin)!=1)return 0;
        for(size_t j=0;j<i;j++){
            struct eo_saved_source *other=g->entries[j].source;
            /* A second alias of the same script is not a second device script.
             * Identical file contents at distinct real identities are allowed. */
            if(s==other||(s->captured[0].st_dev==other->captured[0].st_dev&&
                s->captured[0].st_ino==other->captured[0].st_ino))return 0;
        }
    }
    return 1;
#else
    return 0;
#endif
}
int br_object_schedule_registry_init(br_object_schedule_registry *g,br_schedule_runner *r,
    const br_object_schedule_binding *bindings,size_t count){
    if(!g||g->ready||g->busy||!r||count>BR_SCHEDULE_TASKS||r->count!=count||
       (count&&!bindings)||r->status!=BR_RUN_IDLE||r->candidate||r->run)return BR_OBJECT_INVALID;
    size_t base,total;if(br_schedule_runner_memory(r,&base,&total)!=1)return BR_OBJECT_INVALID;
    br_object_schedule_registry *next=calloc(1,sizeof(*next));if(!next)return BR_OBJECT_INVALID;
    g->busy=1;next->runner=r;next->count=count;next->ready=1;int rc=BR_OBJECT_INVALID;
    for(size_t i=0;i<count;i++){
        const br_object_schedule_binding *b=bindings+i;
        if(!b->owner||!b->object||!b->schedule||b->object->held||b->owner->in_flight||
           r->scripts[i]!=b->schedule||!bound_at(b->object,r,i))goto done;
        next->entries[i].binding=*b;next->entries[i].origin=b->object->origin;
        memcpy(next->entries[i].script,b->schedule->script,sizeof(next->entries[i].script));
        memcpy(next->entries[i].revision,b->schedule->revision,sizeof(next->entries[i].revision));
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
        next->entries[i].source=b->owner->schedule_source;
#endif
    }
    if(!registry_current(next))goto done;
    *g=*next;rc=BR_OBJECT_OK;
done:memset(next,0,sizeof(*next));free(next);g->busy=0;return rc;
}
int br_object_schedule_registry_reset(br_object_schedule_registry *g){
    if(!g||!g->ready||g->busy||!g->runner||g->runner->status!=BR_RUN_IDLE||
       g->runner->candidate||g->runner->run||g->count>BR_SCHEDULE_TASKS)return BR_OBJECT_INVALID;
    for(size_t i=0;i<g->count;i++){
        const br_object_schedule_binding *b=&g->entries[i].binding;
        if(!b->object||!b->owner||!b->schedule||b->object->held||b->owner->in_flight||
           b->owner->busy||b->schedule->owned||!br_schedule_storage_valid(b->schedule))return BR_OBJECT_INVALID;
        for(size_t k=0;k<b->schedule->count;k++){
            enum br_schedule_phase phase=b->schedule->records[k].phase;
            if(phase>=BR_SCHED_INTENT&&phase<=BR_SCHED_UNKNOWN)return BR_OBJECT_INVALID;
        }
    }
    if(g->scope_release&&g->scope_release(g->scope)!=1)return BR_OBJECT_INVALID;
    memset(g,0,sizeof(*g));return BR_OBJECT_OK;
}
static void registry_refuse(br_object_schedule_registry *g,const struct eo_clock *clock){
    br_schedule_candidate ignored;
    if(g&&g->runner&&clock)(void)br_schedule_runner_poll(g->runner,0,clock->wall_ms,clock->mono_ms,0,&ignored);
}
static int registry_poll_impl(br_object_schedule_registry *g,const struct eo_clock *clock,br_schedule_candidate *out){
    if(!registry_current(g))goto bad;
    for(size_t i=0;i<g->count;i++)if(g->entries[i].binding.object->held)return 0;
    /* Each original schedule source checks the same explicit clock sample. */
    for(size_t i=0;i<g->count;i++)if(eo_schedule_consume(g->entries[i].binding.owner,
        &g->entries[i].origin,clock,registry_view,&g->entries[i].origin)!=1)goto bad;
    br_schedule_candidate next;
    int rc=br_schedule_runner_poll(g->runner,clock->trusted,clock->wall_ms,clock->mono_ms,clock->idle,&next);
    if(!registry_current(g))goto bad;
    if(rc==1)*out=next;return rc;
bad:registry_refuse(g,clock);return BR_OBJECT_INVALID;
}
int br_object_schedule_registry_poll(br_object_schedule_registry *g,const struct eo_clock *clock,br_schedule_candidate *out){
    if(!g||g->busy||!clock||!out)return BR_OBJECT_INVALID;
    g->busy=1;int rc=registry_poll_impl(g,clock,out);g->busy=0;return rc;
}
typedef struct { br_object_schedule_registry *g;br_schedule_run_io original; } registry_io;
static int registry_acquire(void *p,uint64_t run){
    registry_io *q=p;int rc=q->original.acquire(q->original.context,run);
    /* Acquired ownership cannot be described as a proven unsent/no-owner
     * refusal. Returning uncertain delegates original close/drain handling. */
    return rc==1&&!registry_current(q->g)?-1:rc;
}
static int registry_send(void *p,uint64_t run,uint64_t epoch,const void *bytes,size_t length){
    registry_io *q=p;
    /* This wrapper has sent zero bytes when a source becomes stale while the
     * original durable INTENT was being written. Original retry/drain owns it. */
    if(!registry_current(q->g))return 0;
    return q->original.send(q->original.context,run,epoch,bytes,length);
}
static void registry_close(void *p,uint64_t run,uint64_t epoch,uint64_t now){
    registry_io *q=p;q->original.close(q->original.context,run,epoch,now);
}
static int registry_reaped(void *p,uint64_t run,uint64_t epoch){
    registry_io *q=p;return q->original.reaped(q->original.context,run,epoch);
}
int br_object_schedule_registry_start(br_object_schedule_registry *g,const struct eo_clock *clock,
    uint64_t run,uint64_t epoch,const char *uuid,char *open,size_t cap){
    if(!g||g->busy||!clock||!uuid||!open||cap<8193||!g->runner)return BR_OBJECT_INVALID;
    g->busy=1;br_schedule_runner *r=g->runner;int rc=BR_OBJECT_INVALID;
    if(!r->candidate)goto done;
    br_schedule_candidate before=r->next,c={0,0};
    if(registry_poll_impl(g,clock,&c)!=1||c.script!=before.script||c.slot!=before.slot||
       c.script>=g->count||c.slot>=g->entries[c.script].binding.object->count)goto done;
    br_object_schedule_binding b=g->entries[c.script].binding;
    registry_io q={g,r->io};
    /* Only a synchronous call frame borrows q; restore every original callback
     * before returning. Async receive/tick/recovery keep the original owner. */
    r->io=(br_schedule_run_io){&q,registry_acquire,registry_send,registry_close,registry_reaped};
    rc=start_resolved(b.object,b.owner,r,clock,c,run,epoch,uuid,open,cap);
    r->io=q.original;
done:g->busy=0;return rc;
}
int br_object_schedule_registry_release(br_object_schedule_registry *g,unsigned index,br_inbox *inbox,br_result *scratch){
    if(!g||!g->ready||g->busy||!g->runner||index>=g->count||g->runner->count!=g->count)return BR_OBJECT_INVALID;
    br_object_schedule_binding b=g->entries[index].binding;
    if(g->runner->scripts[index]!=b.schedule||!bound_at(b.object,g->runner,index)||
       !eo_same(&b.object->origin,&g->entries[index].origin)||
       strcmp(b.schedule->script,g->entries[index].script)||strcmp(b.schedule->revision,g->entries[index].revision))return BR_OBJECT_INVALID;
    g->busy=1;int rc=release_at(b.object,b.owner,g->runner,index,inbox,scratch);g->busy=0;return rc;
}
static void hex_digest(char out[65],const unsigned char digest[32]){
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];}out[64]=0;
}
int br_object_schedule_start_session(br_object_schedule *o,struct eo_owner *owner,
    struct eo_owner *flow_owner,const struct eo_origin *source,br_object_flow *copy,
    br_schedule_runner *r,const struct eo_clock *clock,br_schedule_session *x,br_flow_session *a,
    br_flow_session_log *log,br_flow_owner_store *store,uint64_t run,uint64_t generation,
    const char *uuid,br_flow_owner_handoff_current previous,void *context){
    if(!o||!owner||!flow_owner||owner==flow_owner||!source||!copy||!r||!clock||!x||!a||!log||!store||
       !previous||o->held||!bound(o,r)||!r->candidate||r->next.slot>=o->count||
       strcmp(o->origin.script,source->script)||strcmp(o->origin.settings,source->settings)||
       o->origin.revision!=source->revision)return BR_OBJECT_INVALID;
    br_schedule_candidate before=r->next,c={0,0};
    if(br_object_schedule_poll(o,owner,r,clock,&c)!=1||c.script!=before.script||c.slot!=before.slot)return BR_OBJECT_INVALID;
    const char *name=o->run[c.slot].flow;
    if(!name||o->run[c.slot].inputs)return BR_OBJECT_UNSUPPORTED;
    int rc=br_object_flow_copy(copy,flow_owner,source,name);if(rc!=1)return rc;
    for(size_t i=0;i<copy->definition.count;i++)if(copy->steps[i].audio)return BR_OBJECT_UNSUPPORTED;
    /* Identity is built from separate source families, never substitute the
     * schedule carrier for the independent flow carrier. */
    br_flow_log_binding b={.run=run,.serial=r->scripts[0]->sequence+1,
        .count=(unsigned)copy->definition.count,.audible=o->tasks[c.slot].audible,
        .silent=!o->tasks[c.slot].audible};
    strcpy(b.script,r->scripts[0]->script);strcpy(b.revision,o->revision);strcpy(b.task,o->tasks[c.slot].id);
    unsigned char data[64+128],digest[32];size_t n=strlen(name);
    if(n>128||!generation||!b.serial)return BR_OBJECT_INVALID;
    memcpy(data,source->flow,64);memcpy(data+64,name,n);
    if(!SHA256(data,64+n,digest))return BR_OBJECT_INVALID;
    hex_digest(b.definition_sha256,digest);SHA256((const unsigned char *)"{}",2,digest);hex_digest(b.inputs_sha256,digest);
    const br_flow_session_log_binding *v=&log->binding;
    if(strcmp(v->flow_sha256,source->flow)||strcmp(v->script,b.script)||strcmp(v->revision,b.revision)||
       strcmp(v->definition_sha256,b.definition_sha256)||strcmp(v->inputs_sha256,b.inputs_sha256)||
       v->run!=run||v->session_run!=a->session->run||v->epoch!=a->session->epoch||
       v->generation!=generation||v->source!=BR_LOG_SCHEDULED||v->count!=b.count||
       v->ctl_run||v->cycle||v->job)return BR_OBJECT_INVALID;
    if(br_flow_begin(a->flow,run,clock->mono_ms,&copy->definition,NULL,0,r->scripts[0]->script,
        "scheduled",NULL,&r->op->discovery->scratch)!=1)return BR_OBJECT_INVALID;
    if(b.silent&&br_flow_require_silent(a->flow,run,NULL,0)!=1)return BR_OBJECT_INVALID;
    if(eo_hold(flow_owner,source))return BR_OBJECT_INVALID;
    if(eo_hold(owner,&o->origin)){
        (void)eo_release(flow_owner,source,no_claim,NULL);return BR_OBJECT_INVALID;
    }
    o->held=1;o->slot=c.slot;
    rc=br_schedule_runner_claim_session(r,a->flow,run,c,v->epoch,uuid,clock->mono_ms);
    if(rc!=BR_RUN_WAIT){
        if(r->status==BR_RUN_IDLE&&r->run==0){
            if(!eo_release(owner,&o->origin,no_claim,NULL))o->held=0;
            (void)eo_release(flow_owner,source,no_claim,NULL);
        }
        return rc;
    }
    o->serial=r->serial;o->epoch=r->epoch;memcpy(o->command,r->command,37);
    return br_schedule_session_attach(x,r,a,log,store,&b,previous,context)==1?BR_RUN_WAIT:BR_OBJECT_UNVERIFIED;
}
