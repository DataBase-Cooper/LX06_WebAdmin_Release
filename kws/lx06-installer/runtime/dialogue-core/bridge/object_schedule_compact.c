#include "object_schedule_compact.h"
#include "schedule-hash.h"
#include <string.h>
#include <stdio.h>
_Static_assert(sizeof(br_compact_schedule)<=4608,"one global task/ref/binding pool");
static int compact_revision(struct eo_compact_schedule_source *s,struct eo_origin *origin,char out[65]){
    if(eo_compact_schedule_origin(s,origin)!=1||!eo_origin_valid(origin))return 0;
    unsigned char bytes[200],digest[32];static const char hex[]="0123456789abcdef";
    memcpy(bytes,origin->script,64);memcpy(bytes+64,origin->flow,64);memcpy(bytes+128,origin->settings,64);
    for(unsigned i=0;i<8;i++)bytes[192+i]=(unsigned char)(origin->revision>>(i*8));
    struct eo_schedule_hash hash;
    int ok=eo_schedule_hash_init(&hash)&&eo_schedule_hash_update(&hash,bytes,sizeof bytes)&&
        eo_schedule_hash_done(&hash,digest);
    memset(&hash,0,sizeof hash);
    if(!ok){memset(bytes,0,sizeof bytes);memset(digest,0,sizeof digest);return 0;}
    for(unsigned i=0;i<32;i++){out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];}out[64]=0;
    memset(bytes,0,sizeof bytes);memset(digest,0,sizeof digest);return 1;
}
typedef struct { br_compact_schedule *set;const uint64_t *seeds;size_t first,count;int failed; } compact_stage;
static int compact_emit(void *context,unsigned index,const struct eo_task *a,const struct eo_schedule_run_ref *ref){
    compact_stage *stage=context;br_compact_schedule *set=stage->set;
    if(stage->failed||!a||!ref||index!=stage->count||set->task_count>=32||index>=32||
       !stage->seeds[index]||ref->ordinal!=index||ref->reserved||
       (ref->kind!=EO_SCHEDULE_ACTION&&ref->kind!=EO_SCHEDULE_FLOW)||!ref->length){stage->failed=1;return -1;}
    br_schedule_task task={0};memcpy(task.id,a->id,sizeof task.id);
    if(!memchr(task.id,0,sizeof task.id)||!eo_ident(task.id)){stage->failed=1;return -1;}
    task.enabled=a->enabled;task.audible=a->audible;task.grace_s=a->grace_s;task.announce_ttl_s=a->announce_ttl_s;task.seed=stage->seeds[index];
    task.calendar.kind=(enum br_calendar_kind)a->calendar;task.calendar.first_minute=a->first_minute;task.calendar.last_minute=a->last_minute;
    task.calendar.interval_days=a->interval_days;
    if(a->calendar==BR_CAL_EVERY_N&&br_calendar_day(a->year,a->month,a->day,&task.calendar.anchor_day)!=1){stage->failed=1;return -1;}
    if(a->calendar==BR_CAL_WEEKLY)task.calendar.weekdays=a->days;
    if(a->calendar==BR_CAL_MONTHLY)task.calendar.monthdays=a->days;
    if(!br_calendar_valid(&task.calendar)||task.enabled>1||task.audible>1||task.grace_s>86400||
       task.announce_ttl_s<60||task.announce_ttl_s>86400){stage->failed=1;return -1;}
    for(size_t k=stage->first;k<set->task_count;k++)if(!strcmp(set->tasks[k].id,task.id)){stage->failed=1;return -1;}
    set->tasks[set->task_count]=task;set->runs[set->task_count++]=*ref;stage->count++;return 1;
}
int br_compact_schedule_load(br_compact_schedule *set,union eo_compact_schedule_work *work,
    struct eo_compact_schedule_root *root,const char *script,uint64_t revision,const uint64_t seeds[32],eo_nfc nfc,void *context){
    if(!set||set->ready||set->busy||set->held||set->closing||!work||!root||!script||!seeds||!nfc||
       set->count>32||set->task_count>32||(set->work&&set->work!=work)||(set->root&&set->root!=root))return -1;
    set->busy=1;compact_stage stage={set,seeds,set->task_count,0,0};struct eo_compact_schedule_source *source=NULL;
    int rc=eo_compact_schedule_load(work,root,script,revision,nfc,context,compact_emit,&stage,&source);
    if(rc!=1||stage.failed||!source||eo_compact_schedule_count(source)!=stage.count||
       strcmp(eo_compact_schedule_name(source),script)||(stage.count&&set->count==32))goto failed;
    for(size_t i=0;i<set->count;i++)if(!strcmp(eo_compact_schedule_name(set->scripts[i].source),script))goto failed;
    if(!stage.count){rc=eo_compact_schedule_close(&source)==1?0:-1;if(!rc){set->work=work;set->root=root;}set->busy=0;return rc;}
    set->scripts[set->count].source=source;set->scripts[set->count].first=(unsigned)stage.first;
    set->scripts[set->count].count=(unsigned)stage.count;set->count++;set->work=work;set->root=root;set->busy=0;return 1;
failed:
    if(source)(void)eo_compact_schedule_close(&source);
    memset(set->tasks+stage.first,0,(set->task_count-stage.first)*sizeof(*set->tasks));
    memset(set->runs+stage.first,0,(set->task_count-stage.first)*sizeof(*set->runs));
    set->task_count=stage.first;set->busy=0;return -1;
}
int br_compact_schedule_config(const br_compact_schedule *set,size_t i,const char **script,
    char revision[65],const br_schedule_task **tasks,size_t *count){
    if(!set||set->busy||set->closing||i>=set->count||!script||!revision||!tasks||!count)return -1;
    struct eo_origin origin;
    if(!compact_revision(set->scripts[i].source,&origin,revision))return -1;
    *script=eo_compact_schedule_name(set->scripts[i].source);*tasks=set->tasks+set->scripts[i].first;*count=set->scripts[i].count;
    memset(&origin,0,sizeof origin);return 1;
}
typedef struct {
    br_compact_schedule *set;union eo_compact_schedule_work *work;uint64_t revision;
    br_compact_schedule_seeds seeds;void *seed_context;eo_nfc nfc;void *nfc_context;
} compact_directory_load;
static int compact_load_script(void *context,struct eo_compact_schedule_root *root,const char *script){
    compact_directory_load *load=context;uint64_t seeds[32]={0};
    if(load->seeds(load->seed_context,script,seeds)!=1)return -1;
    int rc=br_compact_schedule_load(load->set,load->work,root,script,load->revision,seeds,load->nfc,load->nfc_context);
    memset(seeds,0,sizeof seeds);return rc>=0?1:-1;
}
int br_compact_schedule_load_directory(br_compact_schedule *set,union eo_compact_schedule_work *work,
    struct eo_compact_schedule_root *root,uint64_t revision,br_compact_schedule_seeds seeds,void *seed_context,
    eo_nfc nfc,void *nfc_context,struct eo_compact_schedule_directory **out){
    if(!set||set->count||set->task_count||set->ready||set->busy||set->held||set->closing||
       set->directory||!work||!root||!revision||!seeds||!nfc||!out||*out)return -1;
    set->work=work;set->root=root;
    compact_directory_load load={set,work,revision,seeds,seed_context,nfc,nfc_context};
    if(eo_compact_schedule_directory_load(root,compact_load_script,&load,out)!=1){
        (void)br_compact_schedule_close(set);return -1;
    }
    set->directory=*out;return 1;
}
static int compact_shape(const br_compact_schedule *set){
    if(!set||!set->ready||set->closing||!set->runner||set->count>32||set->task_count>32||set->runner->count!=set->count||
       !set->scope.context||!set->scope.current||!set->scope.retain||!set->scope.release||(set->count&&!set->work))return 0;
    if(set->state_owner&&br_schedule_state_owner_current(set->state_owner)!=1)return 0;
    size_t tasks=0;
    for(size_t i=0;i<set->count;i++){
        const br_schedule *s=set->scripts[i].schedule;
        if(!s||set->runner->scripts[i]!=s||!set->scripts[i].source||set->scripts[i].first!=tasks||
           !set->scripts[i].count||set->scripts[i].count>32-tasks||s->tasks!=set->tasks+tasks||
           s->count!=set->scripts[i].count||eo_compact_schedule_count(set->scripts[i].source)!=s->count||
           strcmp(s->script,eo_compact_schedule_name(set->scripts[i].source)))return 0;
        for(size_t j=0;j<i;j++)if(s==set->scripts[j].schedule||set->scripts[i].source==set->scripts[j].source)return 0;
        if(set->state_owner&&!br_schedule_state_owner_contains(set->state_owner,s))return 0;
        tasks+=s->count;
    }
    return tasks==set->task_count;
}
static int compact_current(br_compact_schedule *set){
    if(!compact_shape(set)||set->scope.current(set->scope.context)!=1)return 0;
    for(size_t i=0;i<set->count;i++){
        struct eo_origin origin;char revision[65];
        if(!compact_revision(set->scripts[i].source,&origin,revision)||strcmp(revision,set->scripts[i].schedule->revision)||
           eo_compact_schedule_current(set->work,set->scripts[i].source,&origin)!=1)return 0;
        memset(&origin,0,sizeof origin);memset(revision,0,sizeof revision);
    }return 1;
}
int br_compact_schedule_bind(br_compact_schedule *set,br_schedule_runner *runner,br_compact_schedule_scope scope){
    if(!set||set->ready||set->busy||set->held||set->closing||!runner||set->count>32||runner->count!=set->count||
       runner->status!=BR_RUN_IDLE||runner->candidate||runner->run)return -1;
    if(set->directory&&(scope.context!=set->directory||scope.current!=eo_compact_schedule_directory_current||
       scope.retain!=eo_compact_schedule_directory_retain||scope.release!=eo_compact_schedule_directory_unretain))return -1;
    size_t base,total;if(br_schedule_runner_memory(runner,&base,&total)!=1)return -1;
    set->busy=1;set->runner=runner;set->scope=scope;set->ready=1;
    for(size_t i=0;i<set->count;i++)set->scripts[i].schedule=runner->scripts[i];
    if(!compact_current(set)||scope.retain(scope.context)!=1){
        set->ready=0;set->runner=NULL;memset(&set->scope,0,sizeof set->scope);
        for(size_t i=0;i<set->count;i++)set->scripts[i].schedule=NULL;set->busy=0;return -1;
    }
    set->busy=0;return 1;
}
int br_compact_schedule_bind_directory(br_compact_schedule *set,br_schedule_runner *runner){
    if(!set||!set->directory)return -1;
    return br_compact_schedule_bind(set,runner,(br_compact_schedule_scope){set->directory,
        eo_compact_schedule_directory_current,eo_compact_schedule_directory_retain,eo_compact_schedule_directory_unretain});
}
int br_compact_schedule_bind_state_owner(br_compact_schedule *set,br_schedule_state_owner *owner){
    if(!set||set->busy||set->state_owner||!compact_shape(set)||
       !owner||br_schedule_state_owner_current(owner)!=1)return -1;
    for(size_t i=0;i<set->count;i++)if(!br_schedule_state_owner_contains(owner,set->scripts[i].schedule))return -1;
    for(size_t i=0;i<set->count;i++){
        br_schedule *s=set->scripts[i].schedule;
        for(unsigned j=0;j<s->count;j++){
            const struct eo_schedule_run_ref *ref=&set->runs[set->scripts[i].first+j];
            if(br_schedule_state_owner_claim_kind(owner,s,j,ref->kind==EO_SCHEDULE_FLOW)!=1)return -1;
        }
    }
    set->state_owner=owner;return 1;
}
static void compact_refuse(br_compact_schedule *set,const struct eo_clock *clock){
    br_schedule_candidate ignored;
    if(set&&set->runner&&clock)(void)br_schedule_runner_poll(set->runner,0,clock->wall_ms,clock->mono_ms,0,&ignored);
}
static int compact_poll(br_compact_schedule *set,const struct eo_clock *clock,br_schedule_candidate *out){
    if(!compact_current(set)||clock->trusted!=1||clock->idle!=1||clock->wall_ms<946656000000LL||
       clock->wall_ms>=7258089600000LL||!clock->mono_ms)goto bad;
    if(set->held)return 0;
    br_schedule_candidate next;int rc=br_schedule_runner_poll(set->runner,clock->trusted,clock->wall_ms,clock->mono_ms,clock->idle,&next);
    if(!compact_current(set))goto bad;if(rc==1)*out=next;return rc;
bad:compact_refuse(set,clock);return -1;
}
int br_compact_schedule_poll(br_compact_schedule *set,const struct eo_clock *clock,br_schedule_candidate *out){
    if(!set||set->busy||set->session||!clock||!out)return -1;set->busy=1;
    int rc=compact_poll(set,clock,out);set->busy=0;return rc;
}
typedef struct { br_compact_schedule *set;br_schedule_run_io original; } compact_io;
static int compact_acquire(void *p,uint64_t run){
    compact_io *q=p;int rc=q->original.acquire(q->original.context,run);return rc==1&&!compact_current(q->set)?-1:rc;
}
static int compact_send(void *p,uint64_t run,uint64_t epoch,const void *bytes,size_t length){
    compact_io *q=p;if(!compact_current(q->set))return 0;return q->original.send(q->original.context,run,epoch,bytes,length);
}
static void compact_close_run(void *p,uint64_t run,uint64_t epoch,uint64_t now){
    compact_io *q=p;q->original.close(q->original.context,run,epoch,now);
}
static int compact_reaped(void *p,uint64_t run,uint64_t epoch){
    compact_io *q=p;return q->original.reaped(q->original.context,run,epoch);
}
static int compact_no_claim(void *p,const struct eo_origin *origin){(void)p;(void)origin;return 1;}
int br_compact_schedule_start(br_compact_schedule *set,const struct eo_clock *clock,
    uint64_t run,uint64_t epoch,const char *uuid,char *open,size_t capacity){
    if(!set||set->busy||set->held||set->session||!set->runner||!clock||!uuid||!open||capacity<8193)return -1;
    set->busy=1;int rc=-1;br_schedule_runner *r=set->runner;struct eo_origin origin={0};char revision[65]={0};
    if(!r->candidate)goto done;br_schedule_candidate before=r->next,chosen;
    if(compact_poll(set,clock,&chosen)!=1||chosen.script!=before.script||chosen.slot!=before.slot||
       chosen.script>=set->count||chosen.slot>=set->scripts[chosen.script].count)goto done;
    struct eo_compact_schedule_source *source=set->scripts[chosen.script].source;
    const struct eo_schedule_run_ref *ref=&set->runs[set->scripts[chosen.script].first+chosen.slot];
    if(ref->kind!=EO_SCHEDULE_ACTION){rc=BR_OBJECT_UNVERIFIED;goto done;}
    if(!compact_revision(source,&origin,revision)||strcmp(revision,r->scripts[chosen.script]->revision)||strlen(uuid)!=36||!epoch)goto done;
    for(unsigned i=0;i<36;i++)if(!((uuid[i]>='0'&&uuid[i]<='9')||(uuid[i]>='a'&&uuid[i]<='f')||uuid[i]=='-'))goto done;
    const char *script=r->scripts[chosen.script]->script;
    if(!*script||strlen(script)>64)goto done;
    for(const char *p=script;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||
        (*p>='0'&&*p<='9')||strchr("._-",*p)))goto done;
    /* B appends its identity only; it never parses A's canonical run JSON. */
    char suffix[256];int suffix_size=snprintf(suffix,sizeof suffix,",\"script_id\":\"%s\",\"epoch\":%llu,\"command_id\":\"%s\",\"wake_source\":\"scheduled\"}",
        script,(unsigned long long)epoch,uuid);
    if(suffix_size<=0||(size_t)suffix_size>=sizeof suffix)goto done;
    size_t length=0;unsigned kind=0;
    if(eo_compact_schedule_run_render(set->work,source,ref,&origin,open,8193-(size_t)suffix_size,&length,&kind)!=1||
       kind!=EO_SCHEDULE_ACTION||length<2||length>=8193-(size_t)suffix_size||open[0]!='{'||open[length-1]!='}')goto done;
    memcpy(open+length-1,suffix,(size_t)suffix_size+1);length=length-1+(size_t)suffix_size;
    br_discovery *discovery=r->op?r->op->discovery:NULL;br_caps_plan plan;
    if(!discovery||br_caps_open_operation(&discovery->caps,epoch,open,length,&discovery->scratch,&plan)!=BR_CAPS_OK||
       plan.oneway||(!r->scripts[chosen.script]->tasks[chosen.slot].audible&&!plan.silent)){
        rc=BR_OBJECT_UNVERIFIED;goto done;
    }
    if(set->state_owner&&br_schedule_state_owner_claim_kind(set->state_owner,
        r->scripts[chosen.script],chosen.slot,0)!=1)goto done;
    if(eo_compact_schedule_hold(set->work,source,&origin)!=1)goto done;
    set->held=1;set->held_script=chosen.script;set->held_slot=chosen.slot;
    compact_io io={set,r->io};r->io=(br_schedule_run_io){&io,compact_acquire,compact_send,compact_close_run,compact_reaped};
    rc=br_schedule_runner_start(r,run,chosen,epoch,uuid,clock->mono_ms,
        &(br_schedule_command){r->scripts[chosen.script]->revision,r->scripts[chosen.script]->tasks[chosen.slot].id,open,length});
    r->io=io.original;
    if(r->run==run){set->serial=r->serial;set->epoch=r->epoch;memcpy(set->command,r->command,37);}
    else if(r->status==BR_RUN_IDLE&&!r->run&&r->last_run<run&&eo_compact_schedule_release(source,&origin,compact_no_claim,NULL)==1)set->held=0;
done:memset(open,0,capacity);memset(&origin,0,sizeof origin);memset(revision,0,sizeof revision);set->busy=0;return rc;
}
typedef struct { br_compact_schedule *set;br_inbox *inbox;br_result *scratch; } compact_retire;
static int compact_proof(void *p,const struct eo_origin *origin){
    (void)origin;compact_retire *q=p;br_compact_schedule *set=q->set;br_schedule *s=set->scripts[set->held_script].schedule;
    if(set->runner->status!=BR_RUN_IDLE||s->owned||set->held_slot>=s->count||!set->serial)return 0;
    const br_schedule_record *record=s->records+set->held_slot;
    if(record->phase!=BR_SCHED_TERMINAL||record->serial!=set->serial||record->epoch!=set->epoch||strcmp(record->command,set->command))return 0;
    br_result *id=q->scratch;memset(id,0,sizeof(*id));id->origin=1;id->epoch=set->epoch;memcpy(id->command_id,set->command,37);
    strcpy(id->local.script,s->script);strcpy(id->local.revision,s->revision);strcpy(id->local.task,s->tasks[set->held_slot].id);id->local.serial=set->serial;
    int rc=br_inbox_sync_receipt(q->inbox,id,(int)s->tasks[set->held_slot].audible);memset(id,0,sizeof(*id));return rc==1;
}
static int compact_release(br_compact_schedule *set,br_inbox *inbox,br_result *scratch){
    if(scratch)memset(scratch,0,sizeof(*scratch));
    if(!set||set->busy||!set->held||!compact_shape(set)||set->held_script>=set->count||!inbox||!scratch)return -1;
    set->busy=1;struct eo_origin origin;char revision[65];int rc=-1;
    struct eo_compact_schedule_source *source=set->scripts[set->held_script].source;
    if(compact_revision(source,&origin,revision)&&!strcmp(revision,set->scripts[set->held_script].schedule->revision)){
        compact_retire proof={set,inbox,scratch};rc=eo_compact_schedule_release(source,&origin,compact_proof,&proof);
        if(rc==1){set->held=0;set->serial=set->epoch=0;memset(set->command,0,sizeof set->command);}
    }
    memset(scratch,0,sizeof(*scratch));memset(&origin,0,sizeof origin);memset(revision,0,sizeof revision);set->busy=0;return rc;
}
int br_compact_schedule_release(br_compact_schedule *set,br_inbox *inbox,br_result *scratch){
    if(!set||set->session)return -1;return compact_release(set,inbox,scratch);
}
int br_compact_schedule_unbind(br_compact_schedule *set){
    if(!set||set->busy||set->held||set->session||!compact_shape(set)||set->runner->status!=BR_RUN_IDLE||set->runner->candidate||set->runner->run)return -1;
    for(size_t i=0;i<set->count;i++){
        br_schedule *s=set->scripts[i].schedule;if(s->owned||!br_schedule_storage_valid(s))return -1;
        for(size_t k=0;k<s->count;k++)if(s->records[k].phase>=BR_SCHED_INTENT&&s->records[k].phase<=BR_SCHED_UNKNOWN)return -1;
    }
    if(set->scope.release(set->scope.context)!=1)return -1;
    set->ready=0;set->runner=NULL;set->state_owner=NULL;memset(&set->scope,0,sizeof set->scope);
    for(size_t i=0;i<set->count;i++)set->scripts[i].schedule=NULL;return 1;
}
int br_compact_schedule_close(br_compact_schedule *set){
    if(!set||set->ready||set->busy||set->held||set->session||set->count>32)return -1;
    set->closing=1;
    for(size_t i=0;i<set->count;i++)if(set->scripts[i].source&&eo_compact_schedule_close(&set->scripts[i].source)!=1)return -1;
    memset(set,0,sizeof(*set));return 1;
}
size_t br_compact_schedule_bytes(void){return sizeof(br_compact_schedule);}
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
#include "object_schedule_flow.inc"
#endif
