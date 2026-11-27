#define _GNU_SOURCE
#include "schedule_state_owner.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
typedef struct stat64 owner_stat;
extern int __fxstat64(int,int,struct stat64 *);
extern int __fxstatat64(int,int,const char *,struct stat64 *,int);
#define owner_fstat(fd,st) __fxstat64(3,(fd),(st))
#define owner_fstatat(fd,path,st,flags) __fxstatat64(3,(fd),(path),(st),(flags))
#else
typedef struct stat owner_stat;
#define owner_fstat fstat
#define owner_fstatat fstatat
#endif
static const char lease_name[]="schedule-global.lock";
static int component(const char *name){
    if(!name||!name[0])return 0;
    size_t n=0;
    while(name[n]){if(n==255||name[n]=='/')return 0;n++;}
    return strcmp(name,".")&&strcmp(name,"..");
}
static int directory(const owner_stat *st){
    return S_ISDIR(st->st_mode)&&st->st_uid==geteuid()&&(st->st_mode&07777)==0700;
}
static int private_lock(const owner_stat *st){
    return S_ISREG(st->st_mode)&&st->st_uid==geteuid()&&(st->st_mode&07777)==0600&&
        st->st_nlink==1&&st->st_size==0;
}
static int identity(const owner_stat *a,const owner_stat *b){
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino;
}
static int anchored(br_schedule_state_owner *s){
    owner_stat root,named,parent,held,lock;
    return s&&s->root>=0&&s->parent>=0&&s->lock>=0&&component(s->entry)&&
        !owner_fstat(s->root,&root)&&directory(&root)&&
        !owner_fstat(s->parent,&parent)&&directory(&parent)&&
        !owner_fstatat(s->parent,s->entry,&named,AT_SYMLINK_NOFOLLOW)&&directory(&named)&&identity(&root,&named)&&
        !owner_fstat(s->lock,&held)&&private_lock(&held)&&
        !owner_fstatat(s->root,lease_name,&lock,AT_SYMLINK_NOFOLLOW)&&private_lock(&lock)&&identity(&held,&lock);
}
static void close_raw(br_schedule_state_owner *s){
    if(s->lock>=0)close(s->lock);
    if(s->root>=0)close(s->root);
    if(s->parent>=0)close(s->parent);
    *s=(br_schedule_state_owner){.root=-1,.parent=-1,.lock=-1};
}
int br_schedule_state_owner_open(br_schedule_state_owner *s,int root,int parent,const char *entry){
    if(!s||!component(entry))return -1;
    owner_stat a,b,p;
    if(owner_fstat(root,&a)||!directory(&a)||owner_fstat(parent,&p)||!directory(&p)||
       owner_fstatat(parent,entry,&b,AT_SYMLINK_NOFOLLOW)||!directory(&b)||!identity(&a,&b))return -1;
    br_schedule_state_owner next={.root=-1,.parent=-1,.lock=-1};
    memcpy(next.entry,entry,strlen(entry)+1);
    next.root=fcntl(root,F_DUPFD_CLOEXEC,3);next.parent=fcntl(parent,F_DUPFD_CLOEXEC,3);
    if(next.root<0||next.parent<0)goto failed;
    next.lock=openat(next.root,lease_name,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(next.lock<0||!anchored(&next)||flock(next.lock,LOCK_EX|LOCK_NB)||!anchored(&next))goto failed;
    *s=next;return 1;
failed:close_raw(&next);return -1;
}
int br_schedule_state_owner_current(br_schedule_state_owner *s){
    if(!s||s->poisoned||!anchored(s)){if(s)s->poisoned=1;return -1;}
    return 1;
}
int br_schedule_state_owner_close(br_schedule_state_owner *s){
    if(!s||s->root<0||s->parent<0||s->lock<0)return -1;
    if(s->attached||s->reserved_bytes||s->reserved_tasks||s->reserved_history||s->reserved_active)return 0;
    close_raw(s);return 1;
}

static int range_overlap(const void *a,size_t na,const void *b,size_t nb){
    if(!na||!nb)return 0;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<na:x-y<nb;
}
static int add_size(size_t *total,size_t n){if(n>SIZE_MAX-*total)return 0;*total+=n;return 1;}
static size_t greater(size_t a,size_t b){return a>b?a:b;}
static int same_directory(int a,int b){
    owner_stat x,y;return !owner_fstat(a,&x)&&!owner_fstat(b,&y)&&directory(&x)&&directory(&y)&&identity(&x,&y);
}
typedef struct {size_t bytes,tasks,history,active,pair;} owner_cost;
static int effective(const br_schedule_state_owner *o,owner_cost *cost){
    *cost=(owner_cost){.bytes=o->actual.framed_bytes,.tasks=o->actual.tasks,
        .history=o->actual.history,.active=o->actual.active};
    for(size_t i=2;i<o->count;i++){
        const br_schedule_state_binding *b=o->bindings+i;
        if(b->held_frame<b->frame||b->held_tasks<b->tasks||b->held_history<b->history||b->held_active<b->active||
           !add_size(&cost->bytes,b->held_frame-b->frame)||!add_size(&cost->tasks,b->held_tasks-b->tasks)||
           !add_size(&cost->history,b->held_history-b->history)||!add_size(&cost->active,b->held_active-b->active))return 0;
    }
    if(o->cold_hold&&!cost->active)cost->active=1;
    size_t pair=0;
    if(!add_size(&pair,o->bindings[0].held_frame)||!add_size(&pair,o->bindings[1].held_frame))return 0;
    pair=greater(pair,o->cold_pair);
    for(size_t i=2;i<o->count;i++)pair=greater(pair,o->bindings[i].held_pair);
    cost->pair=pair;return add_size(&cost->bytes,pair);
}
static int within(const owner_cost *cost){
    return cost->bytes<=BR_SCHEDULE_GLOBAL_STATE_BYTES&&cost->tasks<=BR_SCHEDULE_TASKS&&
        cost->history<=BR_SCHEDULE_GLOBAL_HISTORY&&cost->active<=1;
}
static int charge(br_schedule_state_owner *o){
    owner_cost cost;size_t actual=o->actual.framed_bytes;
    if(!effective(o,&cost)||!add_size(&actual,o->bindings[0].frame)||!add_size(&actual,o->bindings[1].frame)||
       cost.bytes<actual||cost.tasks<o->actual.tasks||cost.history<o->actual.history||cost.active<o->actual.active)return -1;
    o->state_bytes=actual;o->reserved_bytes=cost.bytes-actual;
    o->reserved_tasks=cost.tasks-o->actual.tasks;o->reserved_history=cost.history-o->actual.history;
    o->reserved_active=cost.active-o->actual.active;o->readonly=!within(&cost);return 1;
}
static int resolve_original(void *context,int directory_fd,br_store **out){
    br_schedule_state_owner *o=context;*out=NULL;
    for(size_t i=2;i<o->count;i++){
        br_schedule_state_binding *b=o->bindings+i;
        if(b->closed)return -1;
        if(same_directory(directory_fd,b->store->directory)){
            if(*out)return -1;
            *out=b->store;
        }
    }
    return *out?1:0;
}
static int observe_original(void *context,int directory_fd,int present,
                            const br_store_observation *store,const br_schedule_observation *state){
    br_schedule_state_owner *o=context;
    for(size_t i=2;i<o->count;i++){
        br_schedule_state_binding *b=o->bindings+i;
        if(!same_directory(directory_fd,b->store->directory))continue;
        if(b->seen||(present&&(strcmp(state->script,b->script)||strcmp(state->revision,b->revision))))return -1;
        b->seen=1;b->seen_frame=present?store->framed_bytes:0;
        b->seen_tasks=present?state->task_count:0;b->seen_history=present?state->history_count:0;
        b->seen_active=present?state->active_count:0;return 1;
    }
    return 1; /* all other zero-task/removed owners still count in totals */
}
int br_schedule_state_owner_reconcile(br_schedule_state_owner *o){
    if(!o||!o->configured||o->busy||o->writing||!o->workspace||o->workspace->busy||
       br_schedule_state_owner_current(o)!=1)return -1;
    o->busy=1;
    for(size_t i=2;i<o->count;i++)o->bindings[i].seen=0;
    br_schedule_catalogue_totals totals;int rc=-1;
    if(br_schedule_state_catalogue_observe(o->root,o->parent,o->entry,o->lock,o->max_entries,
        o->workspace,resolve_original,o,observe_original,o,&totals)!=1)goto done;
    br_store_observation pair[2];
    for(size_t i=0;i<2;i++)if(br_store_observe_owned(o->bindings[i].store,NULL,0,pair+i)<0)goto done;
    for(size_t i=2;i<o->count;i++)if(!o->bindings[i].seen)goto done;
    if(br_schedule_state_owner_current(o)!=1)goto done;
    /* Whole scan and both original global frame reads succeeded; only now
     * publish provisional observations and release proven unused delta. */
    o->actual=totals;
    for(size_t i=0;i<o->count;i++){
        br_schedule_state_binding *b=o->bindings+i;
        if(i<2){b->frame=pair[i].framed_bytes;b->tasks=b->history=b->active=0;}
        else{b->frame=b->seen_frame;b->tasks=b->seen_tasks;b->history=b->seen_history;b->active=b->seen_active;}
        b->held_frame=b->frame;b->held_tasks=b->tasks;b->held_history=b->history;
        b->held_active=greater(b->active,b->schedule&&b->schedule->owned?1u:0u);
        b->pending=b->claiming=0;
    }
    size_t bound_active=0;
    for(size_t i=2;i<o->count;i++)if(!add_size(&bound_active,o->bindings[i].active))goto done;
    o->cold_hold=totals.active>bound_active;
    o->cold_pair=o->cold_hold?BR_SCHEDULE_GLOBAL_PAIR_MAX:0;
    o->dirty=0;o->editing=NULL;memset(o->target_revision,0,sizeof(o->target_revision));
    rc=charge(o);
done:o->busy=0;
    if(rc!=1)o->poisoned=1;
    return rc;
}
static int original_before_write(void *context,const br_store *store,const void *data,size_t length){
    br_schedule_state_binding *b=context;br_schedule_state_owner *o=b->owner;
    if(!o||!o->configured||o->busy||o->writing||b->store!=store||b->closed||
       br_schedule_state_owner_current(o)!=1)return -1;
    size_t old_frame=b->held_frame,old_tasks=b->held_tasks,old_history=b->held_history,old_active=b->held_active;
    int recovery=0;
    if(store->schedule==BR_STORE_SCHEDULE){
        br_schedule_observation state;
        const br_schedule_task *tasks=b->schedule?b->schedule->tasks:NULL;
        size_t count=b->schedule?b->schedule->count:0;
        if(br_schedule_probe_bytes(data,length,tasks,count,&state)!=1||strcmp(state.script,b->script)||
           strcmp(state.revision,o->editing==b?o->target_revision:b->revision))return -1;
        if(state.active_count>b->active&&!b->claiming)return -1;
        if((state.task_count!=b->tasks||state.history_count!=b->history)&&o->editing!=b)return -1;
        recovery=(b->held_active||b->active)&&state.task_count==b->tasks&&state.history_count==b->history&&
            length+24u<=b->held_frame;
        b->held_frame=greater(b->held_frame,length+24u);b->held_tasks=greater(b->held_tasks,state.task_count);
        b->held_history=greater(b->held_history,state.history_count);b->held_active=greater(b->held_active,state.active_count);
    }else{
        owner_cost old;if(!effective(o,&old))return -1;
        recovery=old.active>0;
        b->held_frame=greater(b->held_frame,length+24u);
    }
    owner_cost cost;
    if(!effective(o,&cost)||(!within(&cost)&&(!recovery||
       (store->schedule!=BR_STORE_SCHEDULE&&cost.pair>greater(BR_SCHEDULE_GLOBAL_PAIR_MAX,
            o->bindings[0].frame+o->bindings[1].frame))))){
        b->held_frame=old_frame;b->held_tasks=old_tasks;b->held_history=old_history;b->held_active=old_active;return -1;
    }
    if(charge(o)!=1)return -1;
    o->writing=b;return 1;
}
static int original_after_write(void *context,const br_store *store,int result){
    br_schedule_state_binding *b=context;br_schedule_state_owner *o=b->owner;
    if(!o||o->writing!=b||b->store!=store){if(o)o->poisoned=1;return -1;}
    o->writing=NULL;b->pending=1;o->dirty=1;
    if(result||br_schedule_state_owner_current(o)!=1){o->poisoned=1;return -1;}
    return 1; /* NO nested codec/readback while original workspace is busy */
}
static void original_closing(void *context,const br_store *store){
    br_schedule_state_binding *b=context;(void)store;b->closed=1;b->owner->poisoned=1;
    /* The max reservation stays retained; close is not reap or reconciliation. */
}
static int original_before_change(void *context,const br_schedule *schedule,unsigned change,
                                  const br_schedule_projection *projection,const char *revision,unsigned original_slot){
    br_schedule_state_binding *b=context;br_schedule_state_owner *o=b->owner;
    if(!o||b->schedule!=schedule||o->busy||o->writing||br_schedule_state_owner_current(o)!=1||
       (o->dirty&&br_schedule_state_owner_reconcile(o)!=1)||o->readonly)return -1;
    owner_cost old;if(!effective(o,&old))return -1;
    size_t frame=b->held_frame,tasks=b->held_tasks,history=b->held_history,active=b->held_active,pair=b->held_pair;
    if(change==BR_SCHEDULE_EDIT){
        if(!projection||!revision||o->editing)return -1;
        b->held_frame=greater(frame,projection->payload_bytes+24u);b->held_tasks=greater(tasks,projection->task_count);
        b->held_history=greater(history,projection->history_count);
    }else if(change==BR_SCHEDULE_CLAIM){
        if(old.active||original_slot>=BR_SCHEDULE_TASKS)return -1;
        b->held_active=1;b->held_pair=(b->action_mask&(UINT32_C(1)<<original_slot))?0:BR_SCHEDULE_GLOBAL_PAIR_MAX;
    }else if(change!=BR_SCHEDULE_AUTH)return -1;
    owner_cost next;
    if(!effective(o,&next)||!within(&next)){
        b->held_frame=frame;b->held_tasks=tasks;b->held_history=history;b->held_active=active;b->held_pair=pair;return -1;
    }
    if(change==BR_SCHEDULE_EDIT){o->editing=b;memcpy(o->target_revision,revision,65);b->action_mask=0;}
    if(change==BR_SCHEDULE_CLAIM)b->claiming=1;
    return charge(o);
}
static int original_after_save(void *context,const br_schedule *schedule,int result){
    br_schedule_state_binding *b=context;br_schedule_state_owner *o=b->owner;
    if(b->schedule!=schedule||result!=1||schedule->workspace->busy){o->poisoned=1;return -1;}
    /* The revision field is original live storage and changes only after
     * shared preflight and original reconfigure installed the new binding. */
    b->revision=schedule->revision;
    return br_schedule_state_owner_reconcile(o);
}
static void original_after_release(void *context,const br_schedule *schedule){
    br_schedule_state_binding *b=context;br_schedule_state_owner *o=b->owner;
    if(b->schedule!=schedule||schedule->owned){o->poisoned=1;return;}
    b->held_active=b->active;
    if(!b->active)b->held_pair=0;
    if(br_schedule_state_owner_reconcile(o)!=1)o->poisoned=1;
}
static int add_binding(br_schedule_state_owner *o,br_schedule_state_binding *b,br_store *store,br_schedule *schedule){
    *b=(br_schedule_state_binding){.owner=o,.store=store,.schedule=schedule,
        .script=schedule?schedule->script:NULL,.revision=schedule?schedule->revision:NULL};
    if(schedule&&schedule->owned)b->held_pair=BR_SCHEDULE_GLOBAL_PAIR_MAX;
    b->store_guard=(br_store_guard){b,original_before_write,original_after_write,original_closing};
    b->schedule_guard=(br_schedule_guard){b,original_before_change,original_after_save,original_after_release};
    if(br_store_guard_bind(store,&b->store_guard)!=1)return -1;
    if(schedule&&br_schedule_guard_bind(schedule,&b->schedule_guard)!=1)return -1;
    return 1;
}
int br_schedule_state_owner_bind(br_schedule_state_owner *o,br_schedule_workspace *workspace,
    br_schedule_state_binding *slots,size_t capacity,br_schedule *const *schedules,
    br_store *const *stores,size_t count,br_store *flow,br_store *owner,size_t max_entries){
    if(!o||o->configured||o->attached||!workspace||workspace->busy||!slots||capacity<count+2||
       capacity>BR_SCHEDULE_CATALOGUE_ENTRIES||count>BR_SCHEDULE_CATALOGUE_ENTRIES-2||(count&&(!schedules||!stores))||
       !flow||!owner||flow==owner||flow->schedule!=BR_STORE_FLOW||owner->schedule!=BR_STORE_OWNER||
       !same_directory(flow->directory,owner->directory)||flow->guard||owner->guard||
       !max_entries||max_entries>BR_SCHEDULE_CATALOGUE_ENTRIES||br_schedule_state_owner_current(o)!=1||
       range_overlap(workspace,sizeof(*workspace),o,sizeof(*o))||
       range_overlap(workspace,sizeof(*workspace),flow,sizeof(*flow))||
       range_overlap(workspace,sizeof(*workspace),owner,sizeof(*owner))||
       range_overlap(slots,capacity*sizeof(*slots),schedules,count*sizeof(*schedules))||
       range_overlap(slots,capacity*sizeof(*slots),stores,count*sizeof(*stores))||
       range_overlap(slots,capacity*sizeof(*slots),o,sizeof(*o))||
       range_overlap(slots,capacity*sizeof(*slots),workspace,sizeof(*workspace))||
       range_overlap(slots,capacity*sizeof(*slots),flow,sizeof(*flow))||
       range_overlap(slots,capacity*sizeof(*slots),owner,sizeof(*owner)))return -1;
    for(size_t i=0;i<count;i++){
        br_schedule *s=schedules[i];br_store *store=stores[i];
        if(!s||!store||store->schedule!=BR_STORE_SCHEDULE||s->guard||store->guard||
           s->io.context!=store||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||
           range_overlap(slots,capacity*sizeof(*slots),s,sizeof(*s))||
           range_overlap(slots,capacity*sizeof(*slots),store,sizeof(*store))||
           range_overlap(slots,capacity*sizeof(*slots),s->records,s->capacity*sizeof(*s->records))||
           range_overlap(slots,capacity*sizeof(*slots),s->history,s->history_capacity*sizeof(*s->history))||
           range_overlap(slots,capacity*sizeof(*slots),s->tasks,s->count*sizeof(*s->tasks))||
           range_overlap(slots,capacity*sizeof(*slots),s->workspace,sizeof(*s->workspace))||
           range_overlap(workspace,sizeof(*workspace),s,sizeof(*s))||
           range_overlap(workspace,sizeof(*workspace),store,sizeof(*store))||
           range_overlap(workspace,sizeof(*workspace),s->records,s->capacity*sizeof(*s->records))||
           range_overlap(workspace,sizeof(*workspace),s->history,s->history_capacity*sizeof(*s->history))||
           range_overlap(workspace,sizeof(*workspace),s->tasks,s->count*sizeof(*s->tasks))||
           (workspace!=s->workspace&&range_overlap(workspace,sizeof(*workspace),s->workspace,sizeof(*s->workspace))))return -1;
        for(size_t j=0;j<i;j++)if(schedules[j]==s||stores[j]==store||same_directory(stores[j]->directory,store->directory))return -1;
    }
    o->workspace=workspace;o->bindings=slots;o->capacity=capacity;o->count=count+2;
    o->max_entries=max_entries;o->configured=1;
    if(add_binding(o,slots,flow,NULL)!=1||add_binding(o,slots+1,owner,NULL)!=1)goto failed;
    o->attached=2;
    for(size_t i=0;i<count;i++){
        if(add_binding(o,slots+i+2,stores[i],schedules[i])!=1)goto failed;
        o->attached++;
    }
    return br_schedule_state_owner_reconcile(o);
failed:o->poisoned=1;return -1;
}
int br_schedule_state_owner_claim_kind(br_schedule_state_owner *o,const br_schedule *s,unsigned slot,int flow){
    if(!o||!s||slot>=s->count||flow<0||flow>1||o->busy||o->writing||br_schedule_state_owner_current(o)!=1)return -1;
    for(size_t i=2;i<o->count;i++){
        br_schedule_state_binding *b=o->bindings+i;
        if(b->schedule!=s)continue;
        if(flow)b->action_mask&=~(UINT32_C(1)<<slot);
        else b->action_mask|=UINT32_C(1)<<slot;
        if(s->owned&&s->records[slot].serial==s->owned)b->held_pair=flow?BR_SCHEDULE_GLOBAL_PAIR_MAX:0;
        return charge(o);
    }
    return -1;
}
int br_schedule_state_owner_contains(const br_schedule_state_owner *o,const br_schedule *schedule){
    if(!o||!o->configured||o->poisoned)return 0;
    for(size_t i=2;i<o->count;i++)if(o->bindings[i].schedule==schedule&&
       schedule->guard==&o->bindings[i].schedule_guard&&o->bindings[i].store->guard==&o->bindings[i].store_guard)return 1;
    return 0;
}
int br_schedule_state_owner_unbind(br_schedule_state_owner *o){
    if(!o||!o->configured||o->busy||o->writing||o->dirty||o->editing)return -1;
    if(br_schedule_state_owner_reconcile(o)!=1)return -1;
    if(o->reserved_bytes||o->reserved_tasks||o->reserved_history||o->reserved_active||o->actual.active)return 0;
    for(size_t i=2;i<o->count;i++){
        br_schedule_state_binding *b=o->bindings+i;
        if(b->schedule->owned||b->schedule->workspace->busy||b->pending)return 0;
    }
    for(size_t i=2;i<o->count;i++)if(br_schedule_guard_unbind(o->bindings[i].schedule,&o->bindings[i].schedule_guard)!=1)return -1;
    for(size_t i=0;i<o->count;i++)if(br_store_guard_unbind(o->bindings[i].store,&o->bindings[i].store_guard)!=1)return -1;
    memset(o->bindings,0,o->capacity*sizeof(*o->bindings));
    o->bindings=NULL;o->workspace=NULL;o->capacity=o->count=o->attached=0;o->configured=0;return 1;
}
