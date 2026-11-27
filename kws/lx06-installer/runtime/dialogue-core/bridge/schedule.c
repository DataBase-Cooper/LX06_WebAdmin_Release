#include "schedule.h"
#include <string.h>
#include <limits.h>
_Static_assert(sizeof(br_schedule)<1024,"bounded scheduler header");
_Static_assert(sizeof(br_schedule_record)*BR_SCHEDULE_TASKS+
    sizeof(br_schedule_history)*BR_SCHEDULE_HISTORY<BR_SCHEDULE_BYTES,"migration scratch fits");
typedef struct {unsigned char *p;size_t n,pos;int ok;} codec;
static void bytes(codec *c,void *p,size_t n,int write){
    if(!c->ok||n>c->n-c->pos){c->ok=0;return;}
    if(write==3){c->pos+=n;return;}
    if(write==2){if(memcmp(c->p+c->pos,p,n))c->ok=0;}
    else if(write)memcpy(c->p+c->pos,p,n);else memcpy(p,c->p+c->pos,n);
    c->pos+=n;
}
static uint64_t number(codec *c,uint64_t n,unsigned width,int write){
    unsigned char b[8]={0};
    if(write)for(unsigned i=0;i<width;i++)b[i]=(unsigned char)(n>>(8*i));
    bytes(c,b,width,write);if(write)return n;
    n=0;for(unsigned i=0;i<width;i++)n|=(uint64_t)b[i]<<(8*i);return n;
}
static int bounded(const char *s,size_t max){
    if(!s)return 0;
    for(size_t i=0;i<=max;i++)if(!s[i])return i>0;
    return 0;
}
static int ident(const char *s){
    if(!bounded(s,32))return 0;
    for(unsigned i=0;s[i];i++){
        unsigned c=(unsigned char)s[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
    }
    return 1;
}
static int hex(unsigned c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}
static int revision_ok(const char *s){
    if(!bounded(s,64)||strlen(s)!=64)return 0;
    for(unsigned i=0;i<64;i++)if(!hex((unsigned char)s[i]))return 0;
    return 1;
}
static int uuid_ok(const char *s){
    if(!bounded(s,36)||strlen(s)!=36)return 0;
    for(unsigned i=0;i<36;i++){
        if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}
        else if(!hex((unsigned char)s[i]))return 0;
    }
    return s[14]=='7'&&(s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b');
}
static int active(const br_schedule_record *r){return r->phase>=BR_SCHED_INTENT&&r->phase<=BR_SCHED_UNKNOWN;}
static int overlaps(const void *a,size_t na,const void *b,size_t nb){
    if(!na||!nb)return 0;
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<na:x-y<nb;
}
int br_schedule_storage_valid(const br_schedule *s){
    if(!s||!s->workspace||s->capacity>BR_SCHEDULE_TASKS||s->count>s->capacity||
       (s->capacity&&!s->records)||(s->count&&!s->tasks)||s->history_capacity>BR_SCHEDULE_HISTORY||
       s->history_count>s->history_capacity||(s->history_capacity&&!s->history))return 0;
    const void *p[]={s,s->records,s->workspace,s->tasks,s->history};
    size_t n[]={sizeof(*s),s->capacity*sizeof(*s->records),sizeof(*s->workspace),s->count*sizeof(*s->tasks),
        s->history_capacity*sizeof(*s->history)};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<i;j++)if(overlaps(p[i],n[i],p[j],n[j]))return 0;
    return 1;
}
int br_schedule_storage_separate(const br_schedule *a,const br_schedule *b){
    if(!br_schedule_storage_valid(a)||!br_schedule_storage_valid(b)||a==b)return 0;
    const void *p[]={a,a->records,a->workspace,a->tasks,a->history},
        *q[]={b,b->records,b->workspace,b->tasks,b->history};
    size_t n[]={sizeof(*a),a->capacity*sizeof(*a->records),sizeof(*a->workspace),a->count*sizeof(*a->tasks),
        a->history_capacity*sizeof(*a->history)};
    size_t m[]={sizeof(*b),b->capacity*sizeof(*b->records),sizeof(*b->workspace),b->count*sizeof(*b->tasks),
        b->history_capacity*sizeof(*b->history)};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<5;j++){
        if(i==3&&j==3)continue; /* immutable config may be shared */
        if(i==2&&j==2&&a->workspace==b->workspace)continue;
        if(overlaps(p[i],n[i],q[j],m[j]))return 0;
    }
    return 1;
}
static int available(const br_schedule *s){return s&&!s->blocked&&s->workspace&&!s->workspace->busy;}
static void unlock(br_schedule_workspace *w){memset(w->bytes,0,sizeof(w->bytes));w->busy=0;}
static int tasks_ok(const br_schedule_task *t,size_t count){
    if(count>32||(count&&!t))return 0;
    for(size_t i=0;i<count;i++){
        if(!ident(t[i].id)||!br_calendar_valid(&t[i].calendar)||t[i].enabled>1||
           t[i].audible>1||t[i].grace_s>86400||t[i].announce_ttl_s<60||t[i].announce_ttl_s>86400)return 0;
        for(size_t j=0;j<i;j++)if(!strcmp(t[i].id,t[j].id))return 0;
    }
    return 1;
}
/* One original field walker owns decode, encode, exact comparison, pure
 * measurement and readonly observation. Only decode writes caller records.
 * Probe has no init/recovery/write authority; absent calendars stay explicit. */
static int snapshot_walk(br_schedule *s,unsigned char *buffer,size_t length,int mode,
                         size_t *used,const br_schedule_task *config,size_t config_count,
                         br_schedule_observation *observation){
    int probe=mode==4,write=probe?0:mode,reading=!write;
    codec c={buffer,(write==1||write==3)?BR_SCHEDULE_BYTES:length,0,1};
    char magic[8]="LX06SCH1",script[129]={0},rev[65]={0};
    if(write){memcpy(script,s->script,sizeof(script));memcpy(rev,s->revision,sizeof(rev));}
    bytes(&c,magic,8,write);
    uint64_t version=number(&c,probe?0:s->schema,4,write),count=number(&c,probe?0:s->count,4,write);
    bytes(&c,script,sizeof(script),write);bytes(&c,rev,sizeof(rev),write);
    if(reading&&(!c.ok||memcmp(magic,"LX06SCH1",8)||(version!=1&&version!=2)||count>BR_SCHEDULE_TASKS))return 0;
    if(probe){
        if(!bounded(script,128)||!revision_ok(rev)||(config&&count!=config_count))return 0;
    }else if(reading&&(count!=s->count||memcmp(script,s->script,sizeof(script))||
                       memcmp(rev,s->revision,sizeof(rev))))return 0;
    uint64_t sequence=number(&c,probe?0:s->sequence,8,write);
    if(!probe&&reading){s->schema=(unsigned)version;s->sequence=sequence;}
    unsigned running=0,terminal=0;uint16_t ids[BR_SCHEDULE_TASKS+BR_SCHEDULE_HISTORY];size_t id_count=0;
    for(size_t i=0;i<(size_t)count;i++){
        const br_schedule_task *t=probe?(config?config+i:NULL):s->tasks+i;
        br_schedule_record record={0},*r=&record;
        if(write&&write!=3)record=s->records[i];
        char id[33]={0};if(write)memcpy(id,t->id,strlen(t->id));
        size_t id_offset=c.pos;
        bytes(&c,id,sizeof(id),write);r->seed=number(&c,r->seed,8,write);
        if(reading&&(!c.ok||!ident(id)||!r->seed||(t&&memcmp(id,t->id,strlen(t->id)+1))))return 0;
        if(reading){
            for(size_t j=0;j<id_count;j++)if(!strcmp(id,(const char *)buffer+ids[j]))return 0;
            ids[id_count++]=(uint16_t)id_offset;
        }
        r->phase=(enum br_schedule_phase)number(&c,r->phase,4,write);
        r->day=(int32_t)number(&c,(uint32_t)r->day,4,write);
        r->highwater=(int32_t)number(&c,(uint32_t)r->highwater,4,write);
        r->due=(int64_t)number(&c,(uint64_t)r->due,8,write);
        r->serial=number(&c,r->serial,8,write);r->epoch=number(&c,r->epoch,8,write);
        bytes(&c,r->command,sizeof(r->command),write);bytes(&c,r->remote,sizeof(r->remote),write);
        r->attempts=(unsigned)number(&c,r->attempts,4,write);
        r->auth_blocked=(unsigned)number(&c,r->auth_blocked,4,write);
        r->skipped=number(&c,r->skipped,8,write);
        if(reading){
            int64_t due=0;
            if(!c.ok||r->phase<BR_SCHED_WAITING||r->phase>BR_SCHED_TERMINAL||r->auth_blocked>1||
               r->highwater<10956||r->highwater>=84006||r->serial>sequence||
               r->command[36]||r->remote[128])return 0;
            if(t&&r->phase!=BR_SCHED_WAITING&&
               (br_calendar_when(&t->calendar,r->day,r->seed,&due)!=1||due!=r->due))return 0;
            if(r->phase==BR_SCHED_PENDING&&r->day<=r->highwater)return 0;
            if(active(r)||r->phase==BR_SCHED_TERMINAL){
                if(!r->serial||!r->epoch||!uuid_ok(r->command))return 0;
                if(r->phase==BR_SCHED_RUNNING&&!bounded(r->remote,128))return 0;
                if(active(r)&&r->day<=r->highwater)return 0;
                if(r->phase==BR_SCHED_TERMINAL&&r->highwater<r->day)return 0;
            }
            if(active(r))running++;
            if(r->phase==BR_SCHED_TERMINAL)terminal++;
            if(!probe)s->records[i]=record;
        }
    }
    size_t history_count=0;
    if(version==2){
        uint64_t total=number(&c,probe?0:s->history_count,4,write);
        if(!c.ok||total>(probe?BR_SCHEDULE_HISTORY:s->history_capacity))return 0;
        history_count=(size_t)total;
        for(size_t i=0;i<history_count;i++){
            br_schedule_history history={0},*p=&history;
            if(write&&write!=3)history=s->history[i];
            size_t id_offset=c.pos;
            bytes(&c,p->id,sizeof(p->id),write);
            p->seed=number(&c,p->seed,8,write);
            p->highwater=(int32_t)number(&c,(uint32_t)p->highwater,4,write);
            p->skipped=number(&c,p->skipped,8,write);
            p->auth_blocked=(unsigned)number(&c,p->auth_blocked,4,write);
            if(reading){
                if(!c.ok||!ident(p->id)||!p->seed||p->highwater<10956||p->highwater>=84006||p->auth_blocked>1)return 0;
                for(size_t j=0;j<id_count;j++)if(!strcmp(p->id,(const char *)buffer+ids[j]))return 0;
                ids[id_count++]=(uint16_t)id_offset;
                if(!probe)s->history[i]=history;
            }
        }
    }
    if(!c.ok||((write!=1&&write!=3)&&c.pos!=length)||running>1)return 0;
    if(!probe&&reading)s->history_count=history_count;
    *used=c.pos;
    if(probe){
        br_schedule_observation result={.schema=(unsigned)version,.task_count=(size_t)count,
            .history_count=history_count,.active_count=running,.terminal_count=terminal,
            .payload_bytes=c.pos,.sequence=sequence,.calendar_verified=config||!count};
        memcpy(result.script,script,sizeof(script));memcpy(result.revision,rev,sizeof(rev));
        *observation=result;
    }
    return 1;
}
static int snapshot(br_schedule *s,size_t length,int write,size_t *used){
    return snapshot_walk(s,s->workspace->bytes,length,write,used,NULL,0,NULL);
}
int br_schedule_probe_bytes(const void *data,size_t length,const br_schedule_task *tasks,
                            size_t count,br_schedule_observation *out){
    if(!out)return -1;
    /* Refuse output aliases before zeroing a potentially original wire image. */
    if((data&&overlaps(out,sizeof(*out),data,length))||
       (tasks&&overlaps(out,sizeof(*out),tasks,count<=BR_SCHEDULE_TASKS?count*sizeof(*tasks):0)))return -1;
    memset(out,0,sizeof(*out));
    if(!data||length>BR_SCHEDULE_BYTES||(!tasks&&count)||!tasks_ok(tasks,count))return -1;
    size_t used=0;
    return snapshot_walk(NULL,(unsigned char *)data,length,4,&used,tasks,count,out)?1:-1;
}
int br_schedule_probe_io(br_schedule_io io,br_schedule_workspace *w,const br_schedule_task *tasks,
                         size_t count,br_schedule_observation *out){
    if(!out)return -1;
    if((w&&overlaps(out,sizeof(*out),w,sizeof(*w)))||
       (tasks&&count<=BR_SCHEDULE_TASKS&&overlaps(out,sizeof(*out),tasks,count*sizeof(*tasks))))return -1;
    memset(out,0,sizeof(*out));
    if(!io.read||!w||w->busy||(!tasks&&count)||!tasks_ok(tasks,count)||
       overlaps(w,sizeof(*w),tasks,count*sizeof(*tasks)))return -1;
    w->busy=1;size_t length=0;
    int rc=io.read(io.context,w->bytes,sizeof(w->bytes),&length);
    if(rc==1)rc=br_schedule_probe_bytes(w->bytes,length,tasks,count,out);
    else if(rc!=0)rc=-1;
    unlock(w);return rc;
}
int br_schedule_measure(const br_schedule *s,size_t *length){
    if(!length)return -1;
    if(s&&(overlaps(length,sizeof(*length),s,sizeof(*s))||
           overlaps(length,sizeof(*length),s->records,s->capacity*sizeof(*s->records))||
           overlaps(length,sizeof(*length),s->history,s->history_capacity*sizeof(*s->history))||
           overlaps(length,sizeof(*length),s->workspace,s->workspace?sizeof(*s->workspace):0)||
           overlaps(length,sizeof(*length),s->tasks,s->count*sizeof(*s->tasks))))return -1;
    *length=0;
    if(!br_schedule_storage_valid(s)||s->workspace->busy)return -1;
    size_t used=0;
    if(!snapshot_walk((br_schedule *)s,NULL,0,3,&used,NULL,0,NULL))return -1;
    *length=used;return 1;
}
static int save_locked(br_schedule *s){
    size_t length=0;
    int ok=snapshot(s,0,1,&length)&&s->io.write(s->io.context,s->workspace->bytes,length)==0;
    unlock(s->workspace);
    if(!ok)s->blocked=1;
    if(s->guard&&s->guard->after_save(s->guard->context,s,ok?1:-1)!=1){s->blocked=1;return -1;}
    return ok?1:-1;
}
static int save(br_schedule *s){
    if(s->workspace->busy)return -1;
    s->workspace->busy=1;return save_locked(s);
}
int br_schedule_init_history(br_schedule *s,br_schedule_io io,const char *script,const char *revision,
                     const br_schedule_task *tasks,size_t count,br_schedule_record *records,size_t capacity,
                     br_schedule_history *history,size_t history_capacity,br_schedule_workspace *workspace){
    if(!s||!workspace||workspace->busy)return -1;
    /* Validate aliases BEFORE clearing borrowed memory or caller state. */
    br_schedule layout={.records=records,.capacity=capacity,.tasks=tasks,.count=count,.workspace=workspace,
        .history=history,.history_capacity=history_capacity};
    if(!br_schedule_storage_valid(&layout)||overlaps(s,sizeof(*s),workspace,sizeof(*workspace))||
       overlaps(s,sizeof(*s),records,capacity*sizeof(*records))||
       overlaps(s,sizeof(*s),history,history_capacity*sizeof(*history))||
       overlaps(s,sizeof(*s),tasks,count*sizeof(*tasks)))return -1;
    memset(s,0,sizeof(*s));s->blocked=1;
    if(!io.read||!io.write||!bounded(script,128)||!revision_ok(revision)||!tasks_ok(tasks,count))return -1;
    s->records=records;s->capacity=capacity;s->workspace=workspace;
    s->history=history;s->history_capacity=history_capacity;s->schema=1;
    if(capacity)memset(records,0,capacity*sizeof(*records));
    if(history_capacity)memset(history,0,history_capacity*sizeof(*history));
    s->io=io;s->tasks=tasks;s->count=count;memcpy(s->script,script,strlen(script));memcpy(s->revision,revision,64);
    workspace->busy=1;
    size_t length=0;int rc=io.read(io.context,workspace->bytes,BR_SCHEDULE_BYTES,&length),changed=0;
    if(rc==0){
        for(size_t i=0;i<count;i++){
            if(!tasks[i].seed)goto invalid;
            s->records[i].highwater=10956;s->records[i].seed=tasks[i].seed;
        }
        changed=1;
    }else if(rc==1){
        size_t used=0;
        if(length>BR_SCHEDULE_BYTES||!snapshot(s,length,0,&used))goto invalid;
        for(size_t i=0;i<count;i++)if(active(s->records+i)){
            s->records[i].phase=BR_SCHED_UNKNOWN;s->owned=s->records[i].serial;changed=1;
        }
        for(size_t i=0;i<count;i++)if(s->records[i].phase==BR_SCHED_PENDING&&s->records[i].attempts){
            static const unsigned delays[]={30000,60000,120000,300000};
            unsigned n=s->records[i].attempts;
            s->records[i].retry_delay=delays[n<4?n-1:3];s->records[i].retry_since=UINT64_MAX;
        }
    }else goto invalid;
    s->blocked=0;
    if(changed)return save_locked(s);
    unlock(workspace);return 1;
invalid:
    unlock(workspace);return -1;
}
int br_schedule_init(br_schedule *s,br_schedule_io io,const char *script,const char *revision,
                     const br_schedule_task *tasks,size_t count,br_schedule_record *records,size_t capacity,
                     br_schedule_workspace *workspace){
    return br_schedule_init_history(s,io,script,revision,tasks,count,records,capacity,NULL,0,workspace);
}
int br_schedule_reconciled(br_schedule *s){
    if(!available(s))return -1;
    s->reconciled=1;return 1;
}
static void pending(br_schedule_record *r,int32_t day,int64_t due){
    r->phase=BR_SCHED_PENDING;r->day=day;r->due=due;r->epoch=r->serial=0;
    memset(r->command,0,sizeof(r->command));memset(r->remote,0,sizeof(r->remote));
    r->attempts=0;r->retry_since=r->retry_delay=0;
}
static int plan(br_schedule *s,unsigned slot){
    const br_schedule_task *t=s->tasks+slot;br_schedule_record *r=s->records+slot;
    if(!t->enabled||active(r)||(s->owned&&r->serial==s->owned))return 0;
    int32_t day;int64_t due;int changed=0;
    if(r->phase==BR_SCHED_PENDING&&s->wall>=r->due){
        /* Only latest due after a persisted pending occurrence. First boot
         * starts in WAITING and therefore never fabricates pre-config runs. */
        int rc=br_calendar_find(&t->calendar,s->wall,r->seed,-1,&day,&due);
        if(rc<0)return -1;
        if(rc==1&&day>r->day){
            r->highwater=day-1;if(r->skipped<UINT64_MAX)r->skipped++;
            pending(r,day,due);changed=1;
        }
        if(s->wall-r->due>(int64_t)t->grace_s*1000){
            r->highwater=r->day;r->phase=BR_SCHED_WAITING;
            if(r->skipped<UINT64_MAX)r->skipped++;
            changed=1;
        }
    }
    if(r->phase!=BR_SCHED_PENDING){
        int64_t from=s->wall,minimum=(int64_t)(r->highwater+1)*INT64_C(86400000)-INT64_C(28800000);
        if(from<minimum)from=minimum;
        int32_t check;
        if(br_calendar_wall(from,&check)!=1)return changed; /* calendar exhausted */
        int rc=br_calendar_find(&t->calendar,from,r->seed,1,&day,&due);
        if(rc<0)return -1;
        if(rc==1){pending(r,day,due);changed=1;}
    }
    return changed;
}
int br_schedule_poll(br_schedule *s,int trusted,int64_t wall,uint64_t mono,int idle){
    if(!available(s)||trusted<0||trusted>1||idle<0||idle>1)return -1;
    if(s->sampled&&mono<s->mono){s->blocked=1;return -1;}
    int32_t day;
    if(trusted&&br_calendar_wall(wall,&day)!=1){s->trusted=0;return -1;}
    if(trusted&&s->sampled&&s->trusted){
        /* Both walls are bounded to 200 years; subtraction cannot overflow. */
        int64_t delta=wall-s->wall;
        uint64_t elapsed=mono-s->mono;
        int jump=delta<0?(elapsed>=60000||(uint64_t)(-delta)>=60000-elapsed):
            (elapsed>(uint64_t)delta?elapsed-(uint64_t)delta:(uint64_t)delta-elapsed)>=60000;
        if(jump&&s->replans<UINT64_MAX)s->replans++;
    }
    s->sampled=1;s->mono=mono;s->trusted=trusted;s->idle=idle;
    if(!trusted)return 0;
    s->wall=wall;int changed=0;
    for(unsigned i=0;i<s->count;i++){
        if(s->records[i].retry_since==UINT64_MAX)s->records[i].retry_since=mono;
        int rc=plan(s,i);if(rc<0){s->blocked=1;return -1;}changed|=rc;
    }
    if(changed&&save(s)!=1)return -1;
    if(!idle||!s->reconciled||s->owned)return 0;
    int chosen=-1;
    for(unsigned i=0;i<s->count;i++){
        br_schedule_record *r=s->records+i;
        if(active(r))return 0;
        if(!s->tasks[i].enabled||r->phase!=BR_SCHED_PENDING||r->auth_blocked||wall<r->due||
           (r->retry_delay&&mono-r->retry_since<r->retry_delay))continue;
        if(chosen<0||r->due<s->records[chosen].due)chosen=(int)i;
    }
    return chosen+1;
}
int br_schedule_claim(br_schedule *s,unsigned slot,uint64_t mono,uint64_t epoch,const char *uuid){
    if(!available(s)||slot>=s->count||!epoch||!uuid_ok(uuid))return -1;
    if(!s->trusted||!s->idle||!s->sampled||mono!=s->mono||!s->reconciled||s->owned||s->sequence==UINT64_MAX)return 0;
    br_schedule_record *r=s->records+slot;
    if(!s->tasks[slot].enabled||r->phase!=BR_SCHED_PENDING||r->auth_blocked||s->wall<r->due||
       s->wall-r->due>(int64_t)s->tasks[slot].grace_s*1000||
       (r->retry_delay&&mono-r->retry_since<r->retry_delay))return 0;
    for(unsigned i=0;i<s->count;i++){
        if(active(s->records+i)||!strcmp(s->records[i].command,uuid))return 0;
    }
    if(s->guard&&s->guard->before_change(s->guard->context,s,BR_SCHEDULE_CLAIM,NULL,NULL,slot)!=1)return 0;
    r->serial=++s->sequence;r->epoch=epoch;memcpy(r->command,uuid,37);r->phase=BR_SCHED_INTENT;
    s->owned=r->serial;
    return save(s);
}
static br_schedule_record *lookup(br_schedule *s,uint64_t serial){
    if(!available(s)||!serial)return NULL;
    for(unsigned i=0;i<s->count;i++)if(s->records[i].serial==serial)return s->records+i;
    return NULL;
}
int br_schedule_ack(br_schedule *s,uint64_t serial,uint64_t epoch,const char *uuid,const char *remote){
    br_schedule_record *r=lookup(s,serial);
    if(!r||!uuid_ok(uuid)||!bounded(remote,128)||r->epoch!=epoch||strcmp(r->command,uuid)||
       (r->phase!=BR_SCHED_INTENT&&r->phase!=BR_SCHED_RUNNING))return -1;
    if(r->phase==BR_SCHED_RUNNING)return !strcmp(r->remote,remote)?1:-1;
    memcpy(r->remote,remote,strlen(remote)+1);r->phase=BR_SCHED_RUNNING;return save(s);
}
int br_schedule_failed(br_schedule *s,uint64_t serial,uint64_t mono,int no_send,int auth){
    br_schedule_record *r=lookup(s,serial);
    if(!r||!active(r)||mono<s->mono||no_send<0||no_send>1||auth<0||auth>1)return -1;
    if(no_send&&r->phase!=BR_SCHED_INTENT)return -1;
    if(no_send){
        static const unsigned delays[]={30000,60000,120000,300000};
        r->retry_delay=delays[r->attempts<3?r->attempts:3];r->retry_since=mono;
        if(r->attempts<UINT_MAX)r->attempts++;
        r->phase=BR_SCHED_PENDING;r->auth_blocked=(unsigned)auth;
    }else r->phase=BR_SCHED_UNKNOWN;
    s->mono=mono;return save(s);
}
int br_schedule_auth_reset(br_schedule *s,unsigned slot){
    if(!available(s)||slot>=s->count)return -1;
    if(s->guard&&s->guard->before_change(s->guard->context,s,BR_SCHEDULE_AUTH,NULL,NULL,slot)!=1)return -1;
    s->records[slot].auth_blocked=0;return save(s);
}
int br_schedule_finish(br_schedule *s,uint64_t serial,uint64_t epoch,const char *uuid,int proof){
    br_schedule_record *r=lookup(s,serial);
    if(!r||!uuid_ok(uuid)||r->epoch!=epoch||strcmp(r->command,uuid)||proof!=1||
       (!active(r)&&r->phase!=BR_SCHED_TERMINAL))return -1;
    if(r->phase==BR_SCHED_TERMINAL)return 1;
    r->phase=BR_SCHED_TERMINAL;r->highwater=r->day;return save(s);
}
int br_schedule_release(br_schedule *s,uint64_t serial,int reaped){
    br_schedule_record *r=lookup(s,serial);
    if(!r||s->owned!=serial||reaped!=1||
       (r->phase!=BR_SCHED_TERMINAL&&r->phase!=BR_SCHED_PENDING))return -1;
    s->owned=0;
    if(s->guard)s->guard->after_release(s->guard->context,s);
    return 1;
}
int br_schedule_state_current(br_schedule *s){
    if(!br_schedule_storage_valid(s)||!s->io.read)return -1;
    s->workspace->busy=1;size_t length=0,used=0;
    /* Mode2 compares the ORIGINAL codec image against retained identity;
     * it never decodes untrusted disk fields into the live state. */
    int rc=s->io.read(s->io.context,s->workspace->bytes,BR_SCHEDULE_BYTES,&length)==1&&
        length<=BR_SCHEDULE_BYTES&&snapshot(s,length,2,&used);
    unlock(s->workspace);if(!rc)s->blocked=1;return rc?1:-1;
}
int br_schedule_terminal_current(br_schedule *s,uint64_t serial,uint64_t epoch,const char *command){
    if(!br_schedule_storage_valid(s)||!s->io.read)return -1;
    br_schedule_record *r=lookup(s,serial);
    if(!r||s->owned!=serial||r->phase!=BR_SCHED_TERMINAL||r->epoch!=epoch||
       !uuid_ok(command)||strcmp(r->command,command))return -1;
    return br_schedule_state_current(s);
}
int br_schedule_retired_current(br_schedule *s,uint64_t serial,uint64_t epoch,const char *command){
    if(!br_schedule_storage_valid(s)||!s->io.read)return -1;
    br_schedule_record *r=lookup(s,serial);
    if(!r||s->owned||r->phase!=BR_SCHED_TERMINAL||r->epoch!=epoch||
       !uuid_ok(command)||strcmp(r->command,command))return -1;
    return br_schedule_state_current(s);
}
int br_schedule_deferred_current(br_schedule *s,uint64_t serial,uint64_t epoch,const char *command){
    if(!br_schedule_storage_valid(s)||!s->io.read)return -1;
    br_schedule_record *r=lookup(s,serial);
    if(!r||s->owned||r->phase!=BR_SCHED_PENDING||!r->attempts||!r->retry_delay||r->epoch!=epoch||
       !uuid_ok(command)||strcmp(r->command,command))return -1;
    return br_schedule_state_current(s);
}
typedef struct {unsigned map[BR_SCHEDULE_TASKS];size_t retired;int changed;} reconfiguration;
static int reconfigure_plan(const br_schedule *s,const char *expected,const char *revision,
                            const br_schedule_task *tasks,size_t count,reconfiguration *plan){
    if(!available(s)||!revision_ok(expected)||!revision_ok(revision)||strcmp(expected,s->revision)||
       !strcmp(revision,s->revision)||count>s->capacity||!tasks_ok(tasks,count))return -1;
    if(overlaps(tasks,count*sizeof(*tasks),s->tasks,s->count*sizeof(*tasks))||
       overlaps(tasks,count*sizeof(*tasks),s,sizeof(*s))||
       overlaps(tasks,count*sizeof(*tasks),s->records,s->capacity*sizeof(*s->records))||
       overlaps(tasks,count*sizeof(*tasks),s->history,s->history_capacity*sizeof(*s->history))||
       overlaps(tasks,count*sizeof(*tasks),s->workspace,sizeof(*s->workspace)))return -1;
    unsigned *map=plan->map;size_t retired=s->history_count;
    int changed=count!=s->count;
    for(unsigned i=0;i<count;i++){
        unsigned j;for(j=0;j<s->count;j++)if(!strcmp(tasks[i].id,s->tasks[j].id))break;
        if(j<s->count){map[i]=j;continue;}
        changed=1;
        for(j=0;j<s->history_count;j++)if(!strcmp(tasks[i].id,s->history[j].id))break;
        if(j<s->history_count){map[i]=BR_SCHEDULE_TASKS+j;retired--;}
        else{if(!tasks[i].seed)return -1;map[i]=UINT_MAX;}
    }
    for(unsigned j=0;j<s->count;j++){
        unsigned i;for(i=0;i<count;i++)if(map[i]==j)break;
        if(i==count)retired++;
    }
    if(retired>s->history_capacity||(changed&&!s->history_capacity))return -1;
    if(s->owned)return 0;
    for(unsigned i=0;i<s->count;i++)if(active(s->records+i))return 0;
    plan->retired=retired;plan->changed=changed;return 1;
}
int br_schedule_reconfigure_preflight(const br_schedule *s,const char *expected,const char *revision,
                            const br_schedule_task *tasks,size_t count,br_schedule_projection *out){
    if(!out)return -1;
    if((s&&(overlaps(out,sizeof(*out),s,sizeof(*s))||
            overlaps(out,sizeof(*out),s->records,s->capacity*sizeof(*s->records))||
            overlaps(out,sizeof(*out),s->history,s->history_capacity*sizeof(*s->history))||
            overlaps(out,sizeof(*out),s->workspace,s->workspace?sizeof(*s->workspace):0)||
            overlaps(out,sizeof(*out),s->tasks,s->count*sizeof(*s->tasks))))||
       (tasks&&count<=BR_SCHEDULE_TASKS&&overlaps(out,sizeof(*out),tasks,count*sizeof(*tasks))))return -1;
    memset(out,0,sizeof(*out));
    reconfiguration plan;int rc=reconfigure_plan(s,expected,revision,tasks,count,&plan);
    if(rc!=1)return rc;
    br_schedule shape=*s;
    shape.tasks=tasks;shape.count=count;shape.history_count=plan.retired;
    if(plan.changed)shape.schema=2;
    size_t bytes=0;
    if(br_schedule_measure(&shape,&bytes)!=1)return -1;
    *out=(br_schedule_projection){.task_count=count,.history_count=plan.retired,
        .payload_bytes=bytes,.schema=shape.schema};return 1;
}
int br_schedule_reconfigure(br_schedule *s,const char *expected,const char *revision,
                            const br_schedule_task *tasks,size_t count){
    reconfiguration plan;int rc=reconfigure_plan(s,expected,revision,tasks,count,&plan);
    if(rc!=1)return rc;
    const unsigned *map=plan.map;int changed=plan.changed;
    if(s->guard){
        br_schedule_projection projected;
        if(br_schedule_reconfigure_preflight(s,expected,revision,tasks,count,&projected)!=1||
           s->guard->before_change(s->guard->context,s,BR_SCHEDULE_EDIT,&projected,revision,UINT_MAX)!=1)return -1;
    }
    /* The revision argument is borrowed only for this call. Freeze it before
     * workspace reuse or record replacement can invalidate its storage. */
    char next_revision[65];memcpy(next_revision,revision,sizeof(next_revision));
    s->workspace->busy=1;
    for(unsigned i=0;i<count;i++){
        br_schedule_record next={.seed=tasks[i].seed,.highwater=10956};
        if(map[i]<BR_SCHEDULE_TASKS){
            const br_schedule_record *old=s->records+map[i];
            next.seed=old->seed;next.highwater=old->highwater;
            next.skipped=old->skipped;next.auth_blocked=old->auth_blocked;
        }else if(map[i]!=UINT_MAX){
            const br_schedule_history *old=s->history+(map[i]-BR_SCHEDULE_TASKS);
            next.seed=old->seed;next.highwater=old->highwater;
            next.skipped=old->skipped;next.auth_blocked=old->auth_blocked;
        }
        memcpy(s->workspace->bytes+i*sizeof(next),&next,sizeof(next));
    }
    size_t offset=count*sizeof(*s->records),n=0;
    for(unsigned j=0;j<s->history_count;j++){
        unsigned i;for(i=0;i<count;i++)if(map[i]==BR_SCHEDULE_TASKS+j)break;
        if(i<count)continue;
        memcpy(s->workspace->bytes+offset+n*sizeof(*s->history),s->history+j,sizeof(*s->history));n++;
    }
    for(unsigned j=0;j<s->count;j++){
        unsigned i;for(i=0;i<count;i++)if(map[i]==j)break;
        if(i<count)continue;
        const br_schedule_record *old=s->records+j;
        br_schedule_history h={.seed=old->seed,.highwater=old->highwater,
            .skipped=old->skipped,.auth_blocked=old->auth_blocked};
        memcpy(h.id,s->tasks[j].id,strlen(s->tasks[j].id));
        memcpy(s->workspace->bytes+offset+n*sizeof(h),&h,sizeof(h));n++;
    }
    if(s->capacity)memset(s->records,0,s->capacity*sizeof(*s->records));
    if(count)memcpy(s->records,s->workspace->bytes,count*sizeof(*s->records));
    if(s->history_capacity)memset(s->history,0,s->history_capacity*sizeof(*s->history));
    if(n)memcpy(s->history,s->workspace->bytes+offset,n*sizeof(*s->history));
    s->tasks=tasks;s->count=count;s->history_count=n;
    if(changed)s->schema=2;
    memcpy(s->revision,next_revision,sizeof(next_revision));
    s->reconciled=s->sampled=s->trusted=s->idle=0;s->mono=0;s->wall=0;
    return save_locked(s);
}

int br_schedule_guard_bind(br_schedule *s,const br_schedule_guard *guard){
    if(!available(s)||!br_schedule_storage_valid(s)||s->guard||!guard||
       !guard->before_change||!guard->after_save||!guard->after_release)return -1;
    const void *p[]={s,s->records,s->history,s->workspace,s->tasks};
    size_t n[]={sizeof(*s),s->capacity*sizeof(*s->records),s->history_capacity*sizeof(*s->history),
        sizeof(*s->workspace),s->count*sizeof(*s->tasks)};
    for(unsigned i=0;i<5;i++)if(overlaps(guard,sizeof(*guard),p[i],n[i]))return -1;
    s->guard=guard;return 1;
}
int br_schedule_guard_unbind(br_schedule *s,const br_schedule_guard *guard){
    if(!s||s->guard!=guard||!guard||!s->workspace||s->workspace->busy||s->owned)return -1;
    s->guard=NULL;return 1;
}
