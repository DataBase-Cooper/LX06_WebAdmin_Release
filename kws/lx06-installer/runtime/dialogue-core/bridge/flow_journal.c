#include "flow_journal.h"
#include <string.h>
#include <stdlib.h>
_Static_assert(sizeof(br_flow_journal)<36u*1024u,"bounded durable flow view");
_Static_assert(sizeof(br_flow_receipt_witness)<=64u,"bounded durable receipt witness");
_Static_assert(sizeof(br_flow_no_replay)<=56u,"bounded completion disposition");
typedef struct { unsigned char *bytes;size_t size,pos;int ok; } codec;
/* Mode 0 decodes, 1 encodes, 2 compares original fields without replacement. */
static void raw(codec *c,void *p,size_t n,int write){
    if(!c->ok||n>c->size-c->pos){c->ok=0;return;}
    if(write==2){if(memcmp(c->bytes+c->pos,p,n))c->ok=0;}
    else if(write)memcpy(c->bytes+c->pos,p,n);else memcpy(p,c->bytes+c->pos,n);
    c->pos+=n;
}
static uint64_t integer(codec *c,uint64_t value,unsigned n,int write){
    unsigned char b[8]={0};
    if(write==2&&n<8&&value>=(UINT64_C(1)<<(8*n)))c->ok=0;
    if(write)for(unsigned i=0;i<n;i++)b[i]=(unsigned char)(value>>(8*i));
    raw(c,b,n,write);
    if(!write){value=0;for(unsigned i=0;i<n;i++)value|=(uint64_t)b[i]<<(8*i);}
    return value;
}
static int string_valid(const char *s,size_t capacity,int hash){
    size_t n=0;
    for(;n<capacity&&s[n];n++){
        unsigned v=(unsigned char)s[n];
        if(hash?!((v>='0'&&v<='9')||(v>='a'&&v<='f')):
           !((v>='A'&&v<='Z')||(v>='a'&&v<='z')||(v>='0'&&v<='9')||v=='_'||v=='-'||v=='.'))return 0;
    }
    if(!n||n==capacity||(hash&&n!=64))return 0;
    for(size_t i=n+1;i<capacity;i++)if(s[i])return 0;
    return 1;
}
static int uuid(const char *s){
    if(s[36]||s[14]!='7'||!(s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b'))return 0;
    for(unsigned i=0;i<36;i++){
        unsigned v=(unsigned char)s[i];
        if(i==8||i==13||i==18||i==23){if(v!='-')return 0;}
        else if(!((v>='0'&&v<='9')||(v>='a'&&v<='f')))return 0;
    }
    return 1;
}
static int binding_valid(const br_flow_log_binding *b){
    return b&&string_valid(b->script,sizeof(b->script),0)&&string_valid(b->task,sizeof(b->task),0)&&
        string_valid(b->revision,sizeof(b->revision),1)&&string_valid(b->definition_sha256,sizeof(b->definition_sha256),1)&&
        string_valid(b->inputs_sha256,sizeof(b->inputs_sha256),1)&&b->serial&&b->run&&b->count&&b->count<=16&&
        b->silent<=1&&b->audible<=1;
}
int br_flow_log_binding_valid(const br_flow_log_binding *b){return binding_valid(b);}
static int binding(codec *c,const br_flow_log_binding *b,int write){
    /* Fixed length strings; compare without allocating a second snapshot. */
    const char *strings[]={b->script,b->revision,b->definition_sha256,b->inputs_sha256,b->task};
    const size_t sizes[]={129,65,65,65,33};
    for(unsigned i=0;i<5;i++){
        if(write)raw(c,(void *)strings[i],sizes[i],write);
        else{
            if(sizes[i]>c->size-c->pos||memcmp(c->bytes+c->pos,strings[i],sizes[i]))return 0;
            c->pos+=sizes[i];
        }
    }
    const uint64_t values[]={b->serial,b->run,b->count,b->silent,b->audible};
    for(unsigned i=0;i<5;i++)if(integer(c,values[i],i<2?8:1,write)!=values[i])return 0;
    return c->ok;
}
static int json_part(br_flow_journal *j,const unsigned char *data,size_t n,int declarations){
    br_json *d=j->scratch;
    if(br_json_parse(d,data,n)!=1||br_json_kind(d,1)!=(declarations?BR_J_ARRAY:BR_J_OBJECT))return 0;
    if(declarations){
        unsigned count=0;
        for(unsigned t=br_json_child(d,1);t&&t<br_json_next(d,1);t=br_json_next(d,t)){
            if(++count>16||br_json_kind(d,t)!=BR_J_STRING)return 0;
            br_text s=br_json_text(d,t);
            if(!s.length||s.length>32)return 0;
            for(size_t i=0;i<s.length;i++){
                unsigned v=s.data[i];
                if(!((v>='a'&&v<='z')||(v>='A'&&v<='Z')||v=='_'||(i&&v>='0'&&v<='9')))return 0;
            }
            for(unsigned k=br_json_child(d,1);k<t;k=br_json_next(d,k))if(br_json_equal(d,k,d,t))return 0;
        }
    }else{
        for(unsigned i=1;i<=d->count;i++)if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"phone_token")||
            br_json_key_ci(d,i,"authorization"))return 0;
        unsigned t=br_json_field(d,1,"text"),o=br_json_field(d,1,"json");
        if((t&&br_json_kind(d,t)!=BR_J_STRING)||(o&&br_json_kind(d,o)!=BR_J_OBJECT)||br_json_field(d,1,"code"))return 0;
    }
    return 1;
}
static int records_valid(br_flow_journal *j,const br_flow_log_step *steps,unsigned n,
                         const unsigned char *arena,unsigned used,unsigned phase){
    unsigned offset=0;
    if(!n||n>j->binding.count||used>BR_FLOW_ARENA)return 0;
    for(unsigned i=0;i<n;i++){
        const br_flow_log_step *s=steps+i;const br_flow_record *r=&s->record;
        if(!s->ticket||!r->epoch||!uuid(r->command)||s->local>1||(i&&s->ticket<=steps[i-1].ticket))return 0;
        for(unsigned k=0;k<i;k++)if(!strcmp(r->command,steps[k].record.command)||
            (!s->local&&r->epoch==steps[k].record.epoch))return 0;
        if(r->declarations_length){
            if(s->local||r->declarations!=offset||r->declarations_length>used-offset||
               !json_part(j,arena+offset,r->declarations_length,1))return 0;
            offset+=r->declarations_length;
        }else if(r->declarations)return 0;
        int success=i+1<n||phase==BR_FLOW_SUCCESS||phase==BR_FLOW_REAPED||phase==BR_FLOW_LOG_RETIRED;
        if(success){
            if(!r->result_length||r->result!=offset||r->result_length>used-offset||
               !json_part(j,arena+offset,r->result_length,0))return 0;
            offset+=r->result_length;
        }else if(r->result||r->result_length)return 0;
    }
    return offset==used;
}
static void record(codec *c,br_flow_log_step *s,int write){
    br_flow_record *r=&s->record;
    s->ticket=integer(c,s->ticket,8,write);r->epoch=integer(c,r->epoch,8,write);
    s->local=(unsigned)integer(c,s->local,1,write);raw(c,r->command,37,write);
    r->declarations=(uint16_t)integer(c,r->declarations,2,write);
    r->declarations_length=(uint16_t)integer(c,r->declarations_length,2,write);
    r->result=(uint16_t)integer(c,r->result,2,write);
    r->result_length=(uint16_t)integer(c,r->result_length,2,write);
}
static int witness_valid(const br_flow_receipt_witness *w){
    return w->epoch&&uuid(w->command)&&w->received_ms&&w->expires_ms>w->received_ms&&
        w->expires_ms-w->received_ms>=60000&&w->expires_ms-w->received_ms<=86400000&&
        !((w->expires_ms-w->received_ms)%1000);
}
static void witness(codec *c,br_flow_receipt_witness *w,int write){
    w->epoch=integer(c,w->epoch,8,write);raw(c,w->command,37,write);
    w->received_ms=integer(c,w->received_ms,8,write);w->expires_ms=integer(c,w->expires_ms,8,write);
}
static int no_replay_valid(const br_flow_no_replay *p){
    return p->epoch&&uuid(p->command)&&p->ttl_s>=60&&p->ttl_s<=86400;
}
static void no_replay(codec *c,br_flow_no_replay *p,int write){
    p->epoch=integer(c,p->epoch,8,write);raw(c,p->command,37,write);
    p->ttl_s=(unsigned)integer(c,p->ttl_s,4,write);
}
static void failure(codec *c,br_flow_error_witness *e,int write){
    raw(c,e->task,sizeof(e->task),write);raw(c,e->app_version,sizeof(e->app_version),write);
    raw(c,e->hook_version,sizeof(e->hook_version),write);raw(c,e->code,sizeof(e->code),write);
    e->received_ms=integer(c,e->received_ms,8,write);e->expires_ms=integer(c,e->expires_ms,8,write);
}
static int canonical(const char *s,size_t n){
    size_t i=0;while(i<n&&s[i])i++;
    if(!i||i==n)return 0;
    while(++i<n)if(s[i])return 0;
    return 1;
}
static int failure_valid(br_flow_journal *j,int omitted){
    br_flow_error_witness *e=&j->failure;
    if(j->index>=j->binding.count||j->steps[j->index].local||j->binding.silent!=!j->binding.audible||
       !canonical(e->task,sizeof(e->task))||!canonical(e->app_version,sizeof(e->app_version))||
       !canonical(e->hook_version,sizeof(e->hook_version))||!canonical(e->code,sizeof(e->code))||
       !e->received_ms||e->expires_ms<=e->received_ms||e->expires_ms-e->received_ms<60000||
       e->expires_ms-e->received_ms>86400000||(e->expires_ms-e->received_ms)%1000||
       (omitted?(j->receipt.epoch||!no_replay_valid(&j->no_replay)):!witness_valid(&j->receipt)))return 0;
    /* Reuse K6's UTF-8/14-code validator, not another protocol table/parser. */
    br_result *v=calloc(1,sizeof(*v));if(!v)return 0;
    v->epoch=j->steps[j->index].record.epoch;memcpy(v->command_id,j->steps[j->index].record.command,37);
    memcpy(v->task_id,e->task,sizeof(e->task));memcpy(v->app_version,e->app_version,sizeof(e->app_version));
    memcpy(v->hook_version,e->hook_version,sizeof(e->hook_version));memcpy(v->code,e->code,sizeof(e->code));
    int ok=br_result_valid(v);memset(v,0,sizeof(*v));free(v);return ok;
}
static int decode(br_flow_journal *j,size_t size){
    codec c={j->workspace,size,0,1};
    unsigned version=(unsigned)integer(&c,0,4,0);
    if((version<1||version>8)||!binding(&c,&j->binding,0))return 0;
    j->schema=version;
    j->phase=(unsigned)integer(&c,0,1,0);j->index=(unsigned)integer(&c,0,1,0);
    j->used=(unsigned)integer(&c,0,4,0);j->last_ticket=integer(&c,0,8,0);
    if(version==3||version==4)return c.ok&&c.pos==c.size&&
        j->phase==(version==3?BR_FLOW_LOG_BOUND:BR_FLOW_LOG_UNSENT)&&
        !j->index&&!j->used&&!j->last_ticket;
    if(version==7||version==8){
        if(!c.ok||(j->phase!=BR_FLOW_LOG_ERROR_RETIRED&&(version!=8||j->phase!=BR_FLOW_PREPARE))||
           j->index>=j->binding.count||j->used>BR_FLOW_ARENA)return 0;
        for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,0);
        raw(&c,j->arena,j->used,0);
        if(version==8)no_replay(&c,&j->no_replay,0);else witness(&c,&j->receipt,0);
        failure(&c,&j->failure,0);
        return c.ok&&c.pos==c.size&&j->last_ticket==j->steps[j->index].ticket&&failure_valid(j,version==8)&&
            records_valid(j,j->steps,j->index+1,j->arena,j->used,BR_FLOW_PREPARE);
    }
    if(!c.ok||j->phase<1||j->phase>(version==1?4u:BR_FLOW_LOG_RETIRED)||
       (version==2&&(j->phase!=BR_FLOW_LOG_RETIRED||j->index+1!=j->binding.count))||
       (version>=5&&((j->phase!=BR_FLOW_REAPED&&j->phase!=BR_FLOW_LOG_RETIRED)||
                     j->index+1!=j->binding.count||j->binding.silent!=!j->binding.audible))||
       j->index>=j->binding.count||j->used>BR_FLOW_ARENA)return 0;
    for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,0);
    raw(&c,j->arena,j->used,0);
    if(version==5){witness(&c,&j->receipt,0);if(!witness_valid(&j->receipt))return 0;}
    if(version==6){no_replay(&c,&j->no_replay,0);if(!no_replay_valid(&j->no_replay))return 0;}
    return c.ok&&c.pos==c.size&&j->last_ticket==j->steps[j->index].ticket&&
        records_valid(j,j->steps,j->index+1,j->arena,j->used,j->phase);
}
int br_flow_journal_open(br_flow_journal *j,br_store *store,const br_flow_log_binding *b,
                         void *workspace,size_t capacity,br_json *scratch){
    if(!j||!store||store->schedule!=2||!binding_valid(b)||!workspace||capacity<BR_FLOW_LOG_BYTES||!scratch)return -1;
    memset(j,0,sizeof(*j));j->store=store;j->binding=*b;j->workspace=workspace;j->capacity=capacity;j->scratch=scratch;
    size_t n=0;int rc=br_store_read(store,workspace,BR_FLOW_LOG_BYTES,&n);
    if(rc==1){if(!decode(j,n))rc=-1;else{j->readonly=1;
        if(j->phase==BR_FLOW_LOG_RETIRED||j->phase==BR_FLOW_LOG_UNSENT||j->phase==BR_FLOW_LOG_ERROR_RETIRED)rc=2;}}
    if(rc<0){j->blocked=1;memset(j->arena,0,sizeof(j->arena));memset(j->steps,0,sizeof(j->steps));}
    memset(workspace,0,BR_FLOW_LOG_BYTES);memset(scratch,0,sizeof(*scratch));return rc;
}
/* Caller holds busy for the whole original operation, including its proofs.
 * Never drop that guard to reuse the public read-only verification entry. */
static int verify_image(br_flow_journal *j){
    size_t n=0;int rc=BR_IO;
    if(br_store_read(j->store,j->workspace,BR_FLOW_LOG_BYTES,&n)!=1)goto done;
    codec c={j->workspace,n,0,1};
    (void)integer(&c,j->schema,4,2);
    if(!binding(&c,&j->binding,2))goto done;
    (void)integer(&c,j->phase,1,2);(void)integer(&c,j->index,1,2);
    (void)integer(&c,j->used,4,2);(void)integer(&c,j->last_ticket,8,2);
    if(j->schema!=3&&j->schema!=4){
        for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,2);
        raw(&c,j->arena,j->used,2);
        if(j->schema==5||j->schema==7)witness(&c,&j->receipt,2);
        if(j->schema==6||j->schema==8)no_replay(&c,&j->no_replay,2);
        if(j->schema==7||j->schema==8)failure(&c,&j->failure,2);
    }
    /* Fields absent from this schema cannot supply a forged RAM witness. */
    const unsigned char *absent[]={ (const void *)&j->receipt,(const void *)&j->no_replay,
                                    (const void *)&j->failure };
    const size_t sizes[]={sizeof(j->receipt),sizeof(j->no_replay),sizeof(j->failure)};
    const int present[]={j->schema==5||j->schema==7,j->schema==6||j->schema==8,j->schema>=7};
    for(unsigned k=0;k<3;k++)if(!present[k])for(size_t i=0;i<sizes[k];i++)if(absent[k][i])c.ok=0;
    if(c.ok&&c.pos==c.size)rc=1;
done:
    if(rc!=1)j->blocked=1;
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);memset(j->scratch,0,sizeof(*j->scratch));
    return rc;
}
static int verify(br_flow_journal *j,int live){
    if(!j||j->busy||(live?j->readonly:!j->readonly)||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||
       !binding_valid(&j->binding)||j->schema<1||j->schema>8||j->index>=j->binding.count||
       j->used>BR_FLOW_ARENA)return BR_INVALID;
    if(live==1&&((j->schema!=1&&j->schema!=5)||j->phase!=BR_FLOW_REAPED||
       j->index+1!=j->binding.count||j->no_replay.epoch))return BR_INVALID;
    if(live==2&&!((j->schema==3&&j->phase==BR_FLOW_LOG_BOUND&&
       !j->index&&!j->used&&!j->last_ticket)||(j->schema==1&&
       j->phase>=BR_FLOW_PREPARE&&j->phase<=BR_FLOW_NOT_SENT)))return BR_INVALID;
    if(j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2)return BR_IO;
    j->busy=1;int rc=verify_image(j);j->busy=0;return rc;
}
/* Existing receipt/disposition/retirement writes only. The original K6 and
 * owner checks have already run; exact bytes are an additional prerequisite,
 * never proof of source, release, continuation or replay permission. */
static int write_predecessor(br_flow_journal *j){
    if(!j->busy||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||
       !j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||!binding_valid(&j->binding)||
       j->schema<1||j->schema>8||j->index>=j->binding.count||j->used>BR_FLOW_ARENA){
        j->blocked=1;return 0;
    }
    return verify_image(j)==1;
}
int br_flow_journal_verify(br_flow_journal *j){return verify(j,0);}
int br_flow_journal_verify_live(br_flow_journal *j){return verify(j,1);}
int br_flow_journal_verify_pending(br_flow_journal *j){return verify(j,2);}
static int same_record(const br_flow_record *a,const br_flow_record *b){
    return a->epoch==b->epoch&&!strcmp(a->command,b->command)&&a->declarations==b->declarations&&
        a->declarations_length==b->declarations_length&&a->result==b->result&&a->result_length==b->result_length;
}
int br_flow_journal_bind(br_flow_journal *j,const br_flow_log_binding *b,const br_schedule *s,unsigned slot){
    if(!j||j->busy||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||
       !j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||!binding_valid(b)||
       (j->phase&&j->phase!=BR_FLOW_LOG_RETIRED&&j->phase!=BR_FLOW_LOG_UNSENT&&j->phase!=BR_FLOW_LOG_ERROR_RETIRED)||(!j->phase&&j->readonly)||
       !s||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||slot>=s->count||
       strcmp(s->script,b->script)||strcmp(s->revision,b->revision)||strcmp(s->tasks[slot].id,b->task)||
       s->tasks[slot].audible!=b->audible||b->silent!=!b->audible)return -1;
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_INTENT||r->serial!=b->serial||s->owned!=b->serial||!r->epoch||!uuid(r->command)||
       (j->phase&&(b->run<=j->binding.run||(!strcmp(b->script,j->binding.script)&&b->serial<=j->binding.serial))))return -1;
    /* A RAM tombstone alone must not authorize overwriting another image.
     * Keep the old binding until its exact retired bytes have been read back. */
    if(j->phase&&br_flow_journal_verify(j)!=1){j->blocked=1;return -1;}
    /* Copy permits a caller borrowing the old binding; commit RAM only after
     * disk durability. No results or predecessor credentials enter BOUND. */
    br_flow_log_binding next=*b;j->busy=1;
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,3,4,1);
    int ok=binding(&c,&next,1);
    (void)integer(&c,BR_FLOW_LOG_BOUND,1,1);(void)integer(&c,0,1,1);
    (void)integer(&c,0,4,1);(void)integer(&c,0,8,1);
    if(!ok||!c.ok||br_store_write(j->store,j->workspace,c.pos)){j->blocked=1;ok=0;}
    if(ok){
        j->schema=3;j->binding=next;j->phase=BR_FLOW_LOG_BOUND;j->index=j->used=0;j->last_ticket=0;j->readonly=0;
        memset(j->steps,0,sizeof(j->steps));memset(j->arena,0,sizeof(j->arena));
        memset(&j->receipt,0,sizeof(j->receipt));
        memset(&j->no_replay,0,sizeof(j->no_replay));
        memset(&j->failure,0,sizeof(j->failure));
    }
    memset(&next,0,sizeof(next));memset(j->workspace,0,BR_FLOW_LOG_BYTES);
    memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;return ok?1:-1;
}
int br_flow_journal_checkpoint(void *context,const br_flow *f,unsigned event){
    br_flow_journal *j=context;
    if(!j||j->busy||j->blocked||j->readonly||j->phase==BR_FLOW_LOG_RETIRED||j->receipt.epoch||j->no_replay.epoch||!f)return -1;
    /* Check every existing predecessor, including idempotent calls and changes
     * during start/reaped/receipt. The legacy unbound first PREPARE retains its
     * original absent-store path; scheduled flows already have durable BOUND.
     * Sole serial owner; the original guard covers each read and write. */
    if((j->phase||event==BR_FLOW_SUCCESS)&&br_flow_journal_verify_pending(j)!=1){j->blocked=1;return -1;}
    j->busy=1;int ok=0;unsigned index=event==BR_FLOW_REAPED?f->index-1:f->index;
    if(event<1||event>4||index>=j->binding.count||!f->definition||f->definition->count!=j->binding.count||
       f->run!=j->binding.run||strcmp(f->script_id,j->binding.script)||strcmp(f->wake_source,"scheduled")||
       f->silent!=(int)j->binding.silent||!f->ticket||f->used>BR_FLOW_ARENA)goto end;
    if(f->records[index].epoch!=f->epoch||strcmp(f->records[index].command,f->command))goto end;
    if(j->phase==event&&j->index==index&&j->last_ticket==f->ticket&&j->used==f->used&&
       j->steps[index].local==(unsigned)f->local&&!memcmp(j->arena,f->arena,f->used)){
        int identical=1;
        for(unsigned i=0;i<=index;i++)if(!same_record(f->records+i,&j->steps[i].record))identical=0;
        if(identical){ok=1;goto end;}
    }
    if(event==BR_FLOW_PREPARE){
        if(f->status!=BR_FLOW_NEXT||f->ticket<=j->last_ticket||
           (j->phase&&!(j->phase==BR_FLOW_LOG_BOUND?!index:j->phase==BR_FLOW_REAPED?index==j->index+1:
                        j->phase==BR_FLOW_NOT_SENT&&index==j->index))||(!j->phase&&index))goto end;
        if(j->phase&&j->phase!=BR_FLOW_LOG_BOUND&&(!strcmp(f->command,j->steps[j->index].record.command)||
           (!f->local&&f->epoch==j->steps[j->index].record.epoch)))goto end;
    }else{
        if(index!=j->index||f->ticket!=j->last_ticket||f->epoch!=j->steps[index].record.epoch||
           strcmp(f->command,j->steps[index].record.command))goto end;
        const br_flow_record *a=f->records+index,*b=&j->steps[index].record;
        if(j->steps[index].local!=(unsigned)f->local||a->declarations!=b->declarations||
           a->declarations_length!=b->declarations_length||f->used<j->used||
           memcmp(f->arena,j->arena,j->used))goto end;
        if(event!=BR_FLOW_SUCCESS&&(!same_record(a,b)||f->used!=j->used))goto end;
        if(event==BR_FLOW_SUCCESS&&(j->phase!=BR_FLOW_PREPARE||f->status!=BR_FLOW_WAIT))goto end;
        if(event==BR_FLOW_NOT_SENT&&(j->phase!=BR_FLOW_PREPARE||f->status!=BR_FLOW_NEXT))goto end;
        if(event==BR_FLOW_REAPED&&(j->phase!=BR_FLOW_SUCCESS||f->status!=BR_FLOW_DRAINING))goto end;
    }
    /* Prior references cannot be replaced by any subsequent checkpoint. */
    for(unsigned i=0;i<index;i++)if(!same_record(&f->records[i],&j->steps[i].record))goto end;
    unsigned prefix=index?j->steps[index-1].record.result+j->steps[index-1].record.result_length:0;
    if(prefix>f->used||memcmp(f->arena,j->arena,prefix))goto end;
    /* Encode candidate without mutating the last durable RAM view. */
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,1,4,1);
    if(!binding(&c,&j->binding,1))goto end;
    (void)integer(&c,event,1,1);(void)integer(&c,index,1,1);(void)integer(&c,f->used,4,1);
    (void)integer(&c,f->ticket,8,1);
    for(unsigned i=0;i<=index;i++){
        br_flow_log_step s={i==index?f->ticket:j->steps[i].ticket,i==index?(unsigned)f->local:j->steps[i].local,f->records[i]};
        record(&c,&s,1);
    }
    raw(&c,(void *)f->arena,f->used,1);
    if(!c.ok)goto end;
    /* Decode is bounded and revalidates every JSON part before disk mutation.
     * Failure blocks this handle, so rollback of RAM pointers is unnecessary. */
    if(!decode(j,c.pos)||br_store_write(j->store,j->workspace,c.pos))goto end;
    ok=1;
end:
    if(!ok)j->blocked=1;
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;
    return ok?1:-1;
}
/* Only final success snapshots can carry the witness. Old layouts retain their
 * exact bytes; no pointer/ABI image and no extra queue or JSON result copy. */
static int save_final(br_flow_journal *j,unsigned phase,br_flow_receipt_witness *w,br_flow_no_replay *p){
    if(w->epoch&&p->epoch)return 0;
    if(!write_predecessor(j))return 0;
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,p->epoch?6:w->epoch?5:2,4,1);
    if(!binding(&c,&j->binding,1))return 0;
    (void)integer(&c,phase,1,1);(void)integer(&c,j->index,1,1);
    (void)integer(&c,j->used,4,1);(void)integer(&c,j->last_ticket,8,1);
    for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,1);
    raw(&c,j->arena,j->used,1);if(w->epoch)witness(&c,w,1);
    if(p->epoch)no_replay(&c,p,1);
    if(!c.ok||br_store_write(j->store,j->workspace,c.pos)){j->blocked=1;return 0;}
    j->schema=p->epoch?6:w->epoch?5:2;
    return 1;
}
static int receipt_proof(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
                           br_flow_receipt_witness *out){
    if(j->blocked||!j->store||j->store->poisoned)return BR_IO;
    const br_flow_log_binding *b=&j->binding;
    if(j->store->schedule!=2||!binding_valid(b)||b->silent!=!b->audible||
       !j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||j->phase!=BR_FLOW_REAPED||
       j->index+1!=b->count||j->last_ticket!=j->steps[j->index].ticket||
       !records_valid(j,j->steps,b->count,j->arena,j->used,j->phase)||
       !s||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||slot>=s->count||
       strcmp(s->script,b->script)||strcmp(s->revision,b->revision)||strcmp(s->tasks[slot].id,b->task)||
       s->tasks[slot].audible!=b->audible)return BR_INVALID;
    const br_schedule_record *r=s->records+slot;
    if(r->serial!=b->serial||!r->epoch||!uuid(r->command)||r->remote[0]||
       (r->phase==BR_SCHED_TERMINAL?(s->owned&&s->owned!=b->serial):
        ((r->phase!=BR_SCHED_INTENT&&r->phase!=BR_SCHED_UNKNOWN)||s->owned!=b->serial)))return BR_INVALID;
    if(j->receipt.epoch&&(!witness_valid(&j->receipt)||j->receipt.epoch!=r->epoch||
       strcmp(j->receipt.command,r->command)||j->receipt.expires_ms-j->receipt.received_ms!=
       (uint64_t)s->tasks[slot].announce_ttl_s*1000))return BR_INVALID;
    if(j->no_replay.epoch&&(j->receipt.epoch||!no_replay_valid(&j->no_replay)||
       j->no_replay.epoch!=r->epoch||strcmp(j->no_replay.command,r->command)||
       j->no_replay.ttl_s!=s->tasks[slot].announce_ttl_s))return BR_INVALID;
    br_result *v=calloc(1,sizeof(*v));if(!v)return BR_IO;
    v->origin=1;v->epoch=r->epoch;memcpy(v->command_id,r->command,37);
    memcpy(v->local.script,s->script,sizeof(s->script));memcpy(v->local.revision,s->revision,sizeof(s->revision));
    memcpy(v->local.task,s->tasks[slot].id,sizeof(v->local.task));v->local.serial=r->serial;
    int rc=br_inbox_sync_authorized(q,v);const br_result *result=NULL;
    if(rc==1)rc=br_inbox_sync_result(q,v,(int)b->audible,&result);
    memset(v,0,sizeof(*v));free(v);
    if(rc<0)return rc;
    /* A completion-only disposition is never an accepted receipt. A late row
     * conflicts with this explicit policy; do not silently alter that row. */
    if(j->no_replay.epoch)return rc?BR_INVALID:BR_DEFER_LEDGER;
    if(!rc){if(j->receipt.epoch){*out=j->receipt;return 1;}return BR_DEFER_LEDGER;}
    if(!result||result->code[0])return BR_INVALID;
    for(unsigned i=0;i<q->count;i++)if(&q->rows[i].result==result){
        const br_result_row *row=q->rows+i;
        br_flow_receipt_witness w={.epoch=r->epoch,.received_ms=row->received_ms,.expires_ms=row->expires_ms};
        memcpy(w.command,r->command,37);
        if(row->speech_policy!=BR_SPEECH_SCHEDULED||!witness_valid(&w)||
           w.expires_ms-w.received_ms!=(uint64_t)s->tasks[slot].announce_ttl_s*1000||
           (j->receipt.epoch&&(w.received_ms!=j->receipt.received_ms||w.expires_ms!=j->receipt.expires_ms)))return BR_INVALID;
        *out=w;return 1;
    }
    return BR_INVALID;
}
static int receipt_call(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,int capture){
    if(!j||j->busy||!j->scratch)return BR_INVALID;
    j->busy=1;br_flow_receipt_witness w={0};int rc=receipt_proof(j,s,slot,q,&w);
    if(j->no_replay.epoch){
        rc=capture==2&&rc==BR_DEFER_LEDGER?1:rc==BR_IO?BR_IO:BR_INVALID;
        goto done;
    }
    if(rc==1){
        if(capture!=1)rc=j->receipt.epoch?1:0;
        else if(j->receipt.epoch)rc=BR_DUPLICATE;
        else if(!save_final(j,BR_FLOW_REAPED,&w,&j->no_replay))rc=BR_IO;
        else{j->receipt=w;rc=BR_ACCEPTED;}
    }else if(capture!=1&&rc==BR_DEFER_LEDGER)rc=0;
done:
    memset(&w,0,sizeof(w));
    if(j->workspace&&j->capacity>=BR_FLOW_LOG_BYTES)memset(j->workspace,0,BR_FLOW_LOG_BYTES);
    memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;return rc;
}
int br_flow_journal_capture_receipt(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q){
    return receipt_call(j,s,slot,q,1);
}
int br_flow_journal_receipt_check(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q){
    return receipt_call(j,s,slot,q,0);
}
int br_flow_journal_completion_check(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q){
    return receipt_call(j,s,slot,q,2);
}
int br_flow_journal_resolve_no_replay(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_flow_log_binding *expected,const br_flow_definition *d,const br_flow_binding *inputs,size_t count,
    br_flow_retire_check released,void *context){
    if(!j||j->busy||!released||!j->readonly||j->receipt.epoch)return BR_INVALID;
    size_t n=0;
    if(br_flow_journal_output(j,expected,d,inputs,count,j->workspace,j->capacity,&n)<0)return BR_INVALID;
    /* The output is only validated, never admitted or copied to another queue. */
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);
    j->busy=1;br_flow_receipt_witness w={0};
    int rc=receipt_proof(j,s,slot,q,&w);
    if(rc!=BR_DEFER_LEDGER){rc=rc==BR_IO?BR_IO:BR_INVALID;goto done;}
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_UNKNOWN&&r->phase!=BR_SCHED_TERMINAL){rc=BR_INVALID;goto done;}
    if(released(context,&j->binding,r->epoch,r->command)!=1){rc=BR_INVALID;goto done;}
    if(j->no_replay.epoch){rc=BR_DUPLICATE;goto done;}
    br_flow_no_replay p={.epoch=r->epoch,.ttl_s=s->tasks[slot].announce_ttl_s};
    memcpy(p.command,r->command,37);
    if(!no_replay_valid(&p)){rc=BR_INVALID;goto done;}
    if(!save_final(j,BR_FLOW_REAPED,&w,&p))rc=BR_IO;
    else{j->no_replay=p;rc=BR_ACCEPTED;}
    memset(&p,0,sizeof(p));
done:
    memset(&w,0,sizeof(w));memset(j->workspace,0,BR_FLOW_LOG_BYTES);
    memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;return rc;
}
int br_flow_journal_retire(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
                         br_flow_retire_check released,void *context){
    if(!j||j->busy||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||
       !binding_valid(&j->binding)||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch)return -1;
    if(j->phase==BR_FLOW_LOG_RETIRED)return 1;
    if(!released||j->phase!=BR_FLOW_REAPED||j->index+1!=j->binding.count||!s||!br_schedule_storage_valid(s)||
       s->blocked||s->workspace->busy||s->owned||slot>=s->count||strcmp(s->script,j->binding.script)||
       strcmp(s->revision,j->binding.revision)||strcmp(s->tasks[slot].id,j->binding.task)||
       s->tasks[slot].audible!=j->binding.audible)return -1;
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_TERMINAL||r->serial!=j->binding.serial||!r->epoch||!uuid(r->command))return -1;
    int witnessed=(j->receipt.epoch||j->no_replay.epoch)?br_flow_journal_completion_check(j,s,slot,q):0;
    if(witnessed<0)return -1;
    /* This is a non-realtime path. Avoid a ~3KiB result on the event-loop stack. */
    br_result *identity=calloc(1,sizeof(*identity));if(!identity)return -1;
    identity->origin=1;identity->epoch=r->epoch;memcpy(identity->command_id,r->command,37);
    memcpy(identity->local.script,s->script,sizeof(s->script));
    memcpy(identity->local.revision,s->revision,sizeof(s->revision));
    memcpy(identity->local.task,s->tasks[slot].id,sizeof(identity->local.task));identity->local.serial=r->serial;
    j->busy=1;const br_result *receipt=NULL;
    int proof=released(context,&j->binding,r->epoch,r->command)==1?
        (witnessed?1:br_inbox_sync_result(q,identity,(int)j->binding.audible,&receipt)):0;
    memset(identity,0,sizeof(*identity));free(identity);
    int ok=0;
    if(proof!=1||(!witnessed&&(!receipt||receipt->code[0]))||
       !records_valid(j,j->steps,j->index+1,j->arena,j->used,BR_FLOW_LOG_RETIRED))goto end;
    if(!save_final(j,BR_FLOW_LOG_RETIRED,&j->receipt,&j->no_replay))goto end;
    j->phase=BR_FLOW_LOG_RETIRED;j->readonly=1;ok=1;
end:
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;
    return ok?1:-1;
}
int br_flow_journal_retire_unsent(br_flow_journal *j,const br_schedule *s,unsigned slot,
                                br_flow_retire_check released,void *context){
    if(!j||j->busy||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||
       !binding_valid(&j->binding)||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch)return -1;
    if(j->phase==BR_FLOW_LOG_UNSENT)return 1;
    if(!released||j->phase!=BR_FLOW_LOG_BOUND||j->index||j->used||j->last_ticket||
       !s||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||s->owned||slot>=s->count||
       strcmp(s->script,j->binding.script)||strcmp(s->revision,j->binding.revision)||
       strcmp(s->tasks[slot].id,j->binding.task)||s->tasks[slot].audible!=j->binding.audible||
       j->binding.silent!=!j->binding.audible)return -1;
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_PENDING||!r->attempts||r->serial!=j->binding.serial||!r->epoch||!uuid(r->command))return -1;
    j->busy=1;int ok=0;
    if(released(context,&j->binding,r->epoch,r->command)!=1)goto end;
    if(!write_predecessor(j))goto end;
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,4,4,1);
    if(!binding(&c,&j->binding,1))goto end;
    (void)integer(&c,BR_FLOW_LOG_UNSENT,1,1);(void)integer(&c,0,1,1);
    (void)integer(&c,0,4,1);(void)integer(&c,0,8,1);
    if(!c.ok)goto end;
    if(br_store_write(j->store,j->workspace,c.pos)){j->blocked=1;goto end;}
    j->schema=4;j->phase=BR_FLOW_LOG_UNSENT;j->readonly=1;ok=1;
end:
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;
    return ok?1:-1;
}
static int error_result(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,const br_result **out,int omitted){
    *out=NULL;
    if(!j||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2)return BR_IO;
    const br_flow_log_binding *b=&j->binding;
    if(!binding_valid(b)||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch||
       j->phase!=BR_FLOW_PREPARE||j->index>=b->count||j->receipt.epoch||(!omitted&&j->no_replay.epoch)||
       b->silent!=!b->audible||j->last_ticket!=j->steps[j->index].ticket||j->steps[j->index].local||
       !s||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||slot>=s->count||
       strcmp(s->script,b->script)||strcmp(s->revision,b->revision)||strcmp(s->tasks[slot].id,b->task)||
       s->tasks[slot].audible!=b->audible||!remote||!br_result_valid(remote)||remote->code[0]||remote->text[0])return BR_INVALID;
    const br_schedule_record *r=s->records+slot;const br_flow_record *step=&j->steps[j->index].record;
    if((r->phase!=BR_SCHED_INTENT&&r->phase!=BR_SCHED_UNKNOWN&&r->phase!=BR_SCHED_TERMINAL)||
       (r->phase!=BR_SCHED_TERMINAL?s->owned!=b->serial:(s->owned&&s->owned!=b->serial))||
       r->serial!=b->serial||r->remote[0]||!r->epoch||!uuid(r->command)||
       step->epoch!=remote->epoch||strcmp(step->command,remote->command_id)||
       !records_valid(j,j->steps,j->index+1,j->arena,j->used,BR_FLOW_PREPARE))return BR_INVALID;
    const br_result *error=NULL;int rc=br_inbox_ledger_result(q,remote,&error);
    if(rc!=1)return rc;
    if(!error||!error->code[0])return BR_INVALID;
    for(unsigned i=0;i<q->count;i++)if(&q->rows[i].result==error){
        const br_result_row *row=q->rows+i;
        if(row->audible||!row->received_ms||row->expires_ms<=row->received_ms||
           row->expires_ms-row->received_ms<60000||row->expires_ms-row->received_ms>86400000||
           (row->expires_ms-row->received_ms)%1000)return BR_INVALID;
        *out=error;return 1;
    }
    return BR_INVALID;
}
int br_flow_journal_error_result(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,const br_result **out){
    if(!out)return BR_INVALID;
    *out=NULL;if(!j||j->busy)return BR_INVALID;
    j->busy=1;int rc=error_result(j,s,slot,q,remote,out,0);
    if(j->scratch)memset(j->scratch,0,sizeof(*j->scratch));
    j->busy=0;return rc;
}
static int error_pair(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,const br_result **error,const br_result **aggregate){
    *error=NULL;*aggregate=NULL;
    int rc=error_result(j,s,slot,q,remote,error,0);
    if(rc!=1)return rc;
    const br_schedule_record *r=s->records+slot;
    br_result *id=calloc(1,sizeof(*id));if(!id)return BR_IO;
    id->origin=1;id->epoch=r->epoch;memcpy(id->command_id,r->command,37);
    memcpy(id->local.script,s->script,sizeof(s->script));memcpy(id->local.revision,s->revision,sizeof(s->revision));
    memcpy(id->local.task,s->tasks[slot].id,sizeof(id->local.task));id->local.serial=r->serial;
    rc=br_inbox_sync_authorized(q,id);
    if(rc==1)rc=br_inbox_sync_result(q,id,(int)j->binding.audible,aggregate);
    memset(id,0,sizeof(*id));free(id);
    if(rc!=1)return rc;
    if(!*aggregate||strcmp((*error)->code,(*aggregate)->code)||strcmp((*error)->text,(*aggregate)->text))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++)if(&q->rows[i].result==*aggregate){
        const br_result_row *row=q->rows+i;
        return row->speech_policy==BR_SPEECH_SCHEDULED&&row->received_ms&&row->expires_ms>row->received_ms&&
            row->expires_ms-row->received_ms==(uint64_t)s->tasks[slot].announce_ttl_s*1000?1:BR_INVALID;
    }
    return BR_INVALID;
}
int br_flow_journal_error_receipt_check(br_flow_journal *j,const br_schedule *s,unsigned slot,
    br_inbox *q,const br_result *remote){
    if(!j||j->busy)return BR_INVALID;
    j->busy=1;const br_result *error=NULL,*aggregate=NULL;
    int rc=error_pair(j,s,slot,q,remote,&error,&aggregate);
    if(j->scratch)memset(j->scratch,0,sizeof(*j->scratch));
    j->busy=0;return rc;
}
static int error_no_replay_proof(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_error_witness *e,int saved_only){
    const br_result *error=NULL;int rc=error_result(j,s,slot,q,remote,&error,1);
    if(rc!=1&&(rc!=0||!saved_only))return rc;
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_UNKNOWN&&r->phase!=BR_SCHED_TERMINAL)return BR_INVALID;
    if(saved_only&&(!j->no_replay.epoch||!failure_valid(j,1)||
       strcmp(remote->task_id,j->failure.task)||strcmp(remote->app_version,j->failure.app_version)||
       strcmp(remote->hook_version,j->failure.hook_version)))return BR_INVALID;
    if(rc==0){
        /* A durable disposition elects completion without new admission. It
         * is NOT evidence of source text or a received aggregate. error_result
         * already revalidated original records and CURRENT source authority. */
        *e=j->failure;
    }else{
        strcpy(e->task,error->task_id);strcpy(e->app_version,error->app_version);
        strcpy(e->hook_version,error->hook_version);strcpy(e->code,error->code);
        for(unsigned i=0;i<q->count;i++)if(&q->rows[i].result==error){
            e->received_ms=q->rows[i].received_ms;e->expires_ms=q->rows[i].expires_ms;
        }
    }
    if(j->no_replay.epoch){
        const br_flow_no_replay *p=&j->no_replay;const br_flow_error_witness *saved=&j->failure;
        if(!failure_valid(j,1)||p->epoch!=r->epoch||strcmp(p->command,r->command)||
           p->ttl_s!=s->tasks[slot].announce_ttl_s||strcmp(e->task,saved->task)||
           strcmp(e->app_version,saved->app_version)||strcmp(e->hook_version,saved->hook_version)||
           strcmp(e->code,saved->code)||e->received_ms!=saved->received_ms||e->expires_ms!=saved->expires_ms)
            return BR_INVALID;
    }
    br_result *id=calloc(1,sizeof(*id));if(!id)return BR_IO;
    id->origin=1;id->epoch=r->epoch;memcpy(id->command_id,r->command,37);
    memcpy(id->local.script,s->script,sizeof(s->script));memcpy(id->local.revision,s->revision,sizeof(s->revision));
    memcpy(id->local.task,s->tasks[slot].id,sizeof(id->local.task));id->local.serial=r->serial;
    rc=br_inbox_sync_authorized(q,id);const br_result *aggregate=NULL;
    if(rc==1)rc=br_inbox_sync_result(q,id,(int)j->binding.audible,&aggregate);
    memset(id,0,sizeof(*id));free(id);
    /* Absence never proves never played. This path explicitly elects NO new
     * admission. ANY existing aggregate is a conflict, including terminal. */
    return rc==0?1:rc==BR_IO?BR_IO:BR_INVALID;
}
static int save_error_no_replay(br_flow_journal *j,unsigned phase,br_flow_no_replay *p,br_flow_error_witness *e){
    if(!write_predecessor(j))return 0;
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,8,4,1);
    if(!binding(&c,&j->binding,1))return 0;
    (void)integer(&c,phase,1,1);(void)integer(&c,j->index,1,1);
    (void)integer(&c,j->used,4,1);(void)integer(&c,j->last_ticket,8,1);
    for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,1);
    raw(&c,j->arena,j->used,1);no_replay(&c,p,1);failure(&c,e,1);
    if(!c.ok||br_store_write(j->store,j->workspace,c.pos)){j->blocked=1;return 0;}
    j->schema=8;
    return 1;
}
static int error_no_replay_call(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_retire_check released,void *context,int mode){
    if(!j||j->busy)return BR_INVALID;
    if(j->blocked||!j->store||j->store->poisoned)return BR_IO;
    if(!j->readonly||!j->scratch||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||
       !binding_valid(&j->binding)||j->store->schedule!=2)return BR_INVALID;
    int retiring=mode==2||mode==4,checking=mode==0||mode==3;
    if(retiring&&j->phase==BR_FLOW_LOG_ERROR_RETIRED&&j->no_replay.epoch)return 1;
    if((!checking&&!released)||((checking||mode==4)&&!j->no_replay.epoch))return BR_INVALID;
    j->busy=1;br_flow_error_witness e={0};
    int rc=error_no_replay_proof(j,s,slot,q,remote,&e,mode>=3);
    if(rc!=1){if(!rc&&!checking)rc=BR_DEFER_LEDGER;goto done;}
    if(checking)goto done;
    const br_schedule_record *r=s->records+slot;const br_flow_record *step=&j->steps[j->index].record;
    if(retiring&&(!j->no_replay.epoch||r->phase!=BR_SCHED_TERMINAL||s->owned)){rc=BR_INVALID;goto done;}
    if(released(context,&j->binding,r->epoch,r->command)!=1||
       released(context,&j->binding,step->epoch,step->command)!=1){rc=BR_INVALID;goto done;}
    if(mode==1&&j->no_replay.epoch){rc=BR_DUPLICATE;goto done;}
    br_flow_no_replay p={.epoch=r->epoch,.ttl_s=s->tasks[slot].announce_ttl_s};
    memcpy(p.command,r->command,37);
    if(!no_replay_valid(&p)){rc=BR_INVALID;goto done;}
    unsigned phase=retiring?BR_FLOW_LOG_ERROR_RETIRED:BR_FLOW_PREPARE;
    if(!save_error_no_replay(j,phase,&p,&e))rc=BR_IO;
    else{j->no_replay=p;j->failure=e;j->phase=phase;rc=BR_ACCEPTED;}
    memset(&p,0,sizeof(p));
done:
    memset(&e,0,sizeof(e));memset(j->workspace,0,BR_FLOW_LOG_BYTES);
    memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;return rc;
}
int br_flow_journal_resolve_error_no_replay(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_retire_check released,void *context){
    return error_no_replay_call(j,s,slot,q,remote,released,context,1);
}
int br_flow_journal_error_no_replay_check(br_flow_journal *j,const br_schedule *s,unsigned slot,
    br_inbox *q,const br_result *remote){
    return error_no_replay_call(j,s,slot,q,remote,NULL,NULL,0);
}
int br_flow_journal_retire_error_no_replay(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_retire_check released,void *context){
    return error_no_replay_call(j,s,slot,q,remote,released,context,2);
}
int br_flow_journal_saved_error_no_replay_check(br_flow_journal *j,const br_schedule *s,unsigned slot,
    br_inbox *q,const br_result *remote){
    return error_no_replay_call(j,s,slot,q,remote,NULL,NULL,3);
}
int br_flow_journal_retire_saved_error_no_replay(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_retire_check released,void *context){
    return error_no_replay_call(j,s,slot,q,remote,released,context,4);
}
int br_flow_journal_retire_error(br_flow_journal *j,const br_schedule *s,unsigned slot,br_inbox *q,
    const br_result *remote,br_flow_retire_check released,void *context){
    if(!j||j->busy||j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||
       !binding_valid(&j->binding)||!j->workspace||j->capacity<BR_FLOW_LOG_BYTES||!j->scratch)return -1;
    if(j->phase==BR_FLOW_LOG_ERROR_RETIRED)return 1;
    const br_flow_log_binding *b=&j->binding;
    if(!released||j->phase!=BR_FLOW_PREPARE||j->index>=b->count||j->receipt.epoch||j->no_replay.epoch||
       b->silent!=!b->audible||j->last_ticket!=j->steps[j->index].ticket||j->steps[j->index].local||
       !s||!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||s->owned||slot>=s->count||
       strcmp(s->script,b->script)||strcmp(s->revision,b->revision)||strcmp(s->tasks[slot].id,b->task)||
       s->tasks[slot].audible!=b->audible||!remote||!br_result_valid(remote)||remote->code[0]||remote->text[0])return -1;
    const br_schedule_record *r=s->records+slot;const br_flow_record *step=&j->steps[j->index].record;
    if(r->phase!=BR_SCHED_TERMINAL||r->serial!=b->serial||r->remote[0]||!r->epoch||!uuid(r->command)||
       step->epoch!=remote->epoch||strcmp(step->command,remote->command_id))return -1;
    j->busy=1;int ok=0;br_flow_error_witness e={0};br_flow_receipt_witness w={0};
    const br_result *error=NULL,*aggregate=NULL;
    if(error_pair(j,s,slot,q,remote,&error,&aggregate)!=1)goto done;
    for(unsigned i=0;i<q->count;i++){
        const br_result_row *row=q->rows+i;
        if(&row->result==error){
            if(row->audible)goto done;
            e.received_ms=row->received_ms;e.expires_ms=row->expires_ms;
        }
        if(&row->result==aggregate){
            if(row->speech_policy!=BR_SPEECH_SCHEDULED)goto done;
            w.epoch=r->epoch;memcpy(w.command,r->command,37);w.received_ms=row->received_ms;w.expires_ms=row->expires_ms;
        }
    }
    if(!witness_valid(&w)||w.expires_ms-w.received_ms!=(uint64_t)s->tasks[slot].announce_ttl_s*1000||
       !e.received_ms||e.expires_ms<=e.received_ms||e.expires_ms-e.received_ms<60000||
       e.expires_ms-e.received_ms>86400000||(e.expires_ms-e.received_ms)%1000||
       released(context,b,r->epoch,r->command)!=1||released(context,b,step->epoch,step->command)!=1)goto done;
    strcpy(e.task,error->task_id);strcpy(e.app_version,error->app_version);
    strcpy(e.hook_version,error->hook_version);strcpy(e.code,error->code);
    if(!write_predecessor(j))goto done;
    codec c={j->workspace,BR_FLOW_LOG_BYTES,0,1};(void)integer(&c,7,4,1);
    if(!binding(&c,b,1))goto done;
    (void)integer(&c,BR_FLOW_LOG_ERROR_RETIRED,1,1);(void)integer(&c,j->index,1,1);
    (void)integer(&c,j->used,4,1);(void)integer(&c,j->last_ticket,8,1);
    for(unsigned i=0;i<=j->index;i++)record(&c,j->steps+i,1);
    raw(&c,j->arena,j->used,1);witness(&c,&w,1);failure(&c,&e,1);
    if(!c.ok)goto done;
    if(br_store_write(j->store,j->workspace,c.pos)){j->blocked=1;goto done;}
    j->schema=7;j->failure=e;j->receipt=w;j->phase=BR_FLOW_LOG_ERROR_RETIRED;j->readonly=1;ok=1;
done:
    memset(&e,0,sizeof(e));memset(&w,0,sizeof(w));
    memset(j->workspace,0,BR_FLOW_LOG_BYTES);memset(j->scratch,0,sizeof(*j->scratch));j->busy=0;return ok?1:-1;
}
int br_flow_journal_output(br_flow_journal *j,const br_flow_log_binding *expected,
                             const br_flow_definition *d,const br_flow_binding *inputs,size_t count,
                             void *out,size_t capacity,size_t *length){
    int rc=-1;
    if(!j||!j->scratch||j->busy)return -1;
    const br_flow_log_binding *b=&j->binding;
    if(j->blocked||!j->store||j->store->poisoned||j->store->schedule!=2||!j->readonly||
       !j->workspace||j->capacity<BR_FLOW_LOG_BYTES||j->phase!=BR_FLOW_REAPED||
       !binding_valid(b)||!binding_valid(expected)||j->index+1!=b->count||b->silent!=!b->audible||
       strcmp(b->script,expected->script)||strcmp(b->revision,expected->revision)||strcmp(b->task,expected->task)||
       strcmp(b->definition_sha256,expected->definition_sha256)||strcmp(b->inputs_sha256,expected->inputs_sha256)||
       b->serial!=expected->serial||b->run!=expected->run||b->count!=expected->count||
       b->silent!=expected->silent||b->audible!=expected->audible||
       !d||!d->steps||d->count!=b->count||j->last_ticket!=j->steps[j->index].ticket||
       !records_valid(j,j->steps,b->count,j->arena,j->used,j->phase))goto done;
    br_flow_results view={.definition=d,.inputs=inputs,.input_count=count,
                          .arena=j->arena,.used=j->used,.count=b->count};
    for(unsigned i=0;i<b->count;i++){
        const br_flow_step *step=d->steps+i;
        if(!step->action||step->audio||j->steps[i].local!=(unsigned)!strcmp(step->action,"system.op"))goto done;
        view.records[i]=&j->steps[i].record;
    }
    rc=br_flow_saved_output(&view,j->scratch,out,capacity,length);
done:
    memset(j->scratch,0,sizeof(*j->scratch));return rc;
}
