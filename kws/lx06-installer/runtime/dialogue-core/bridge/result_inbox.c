#include "result_inbox.h"
#include <stdlib.h>
#include <string.h>
#define DAY UINT64_C(86400000)
_Static_assert(sizeof(br_inbox)<=192u*1024u,"bounded inbox memory");
_Static_assert(sizeof(br_result_progress)<=1408,"bounded inbox display copy");
static const char *codes[]={"bridge_unreachable","token_invalid","bridge_connect_timeout","app_not_installed",
    "hook_unavailable","hook_version_mismatch","app_version_unsupported","bridge_version_mismatch",
    "session_missing","search_empty","op_timeout","script_disabled","unsupported","quota_exhausted"};
static int boolean(int n){return n==0||n==1;}
static int utf8(const char *s,size_t capacity,unsigned limit,int nonempty) {
    size_t n=0;unsigned count=0;
    while(n<capacity&&s[n])n++;
    if(n==capacity||(nonempty&&!n))return 0;
    for(size_t i=0;i<n;count++){
        uint32_t x=(unsigned char)s[i++],min=0;unsigned rest=0;
        if(x<32||x==127)return 0;
        if(x<128)continue;
        if(x>=0xc2&&x<=0xdf){rest=1;min=0x80;x&=31;}
        else if(x>=0xe0&&x<=0xef){rest=2;min=0x800;x&=15;}
        else if(x>=0xf0&&x<=0xf4){rest=3;min=0x10000;x&=7;}
        else return 0;
        if(i+rest>n)return 0;
        while(rest--){unsigned b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return 0;x=(x<<6)|(b&63);}
        if(x<min||x>0x10ffff||(x>=0xd800&&x<=0xdfff))return 0;
    }
    return count<=limit;
}
static int body(const br_result *r) {
    if(!r||!r->epoch||!utf8(r->command_id,sizeof r->command_id,128,1)||
       !utf8(r->text,sizeof r->text,512,0)||!utf8(r->code,sizeof r->code,39,0))return 0;
    if(strstr(r->text,"://")||strchr(r->text,'<')||strchr(r->text,'>')||strstr(r->text,"@@token@@")||strstr(r->text,"@@TOKEN@@"))return 0;
    if(r->code[0]){unsigned i;for(i=0;i<sizeof codes/sizeof *codes;i++)if(!strcmp(r->code,codes[i]))break;
        if(i==sizeof codes/sizeof *codes)return 0;}
    return 1;
}
int br_result_valid(const br_result *r) {
    return body(r)&&r->origin==0&&!r->local.script[0]&&!r->local.revision[0]&&!r->local.task[0]&&!r->local.serial&&
        utf8(r->task_id,sizeof r->task_id,128,1)&&utf8(r->app_version,sizeof r->app_version,128,1)&&
        utf8(r->hook_version,sizeof r->hook_version,128,1);
}
static int local_valid(const br_result *r){
    if(!body(r)||(r->origin!=1&&r->origin!=2)||(r->origin==2&&(r->code[0]||r->text[0]))||
       r->task_id[0]||r->app_version[0]||r->hook_version[0]||!r->local.serial||
       !utf8(r->local.script,sizeof r->local.script,128,1)||
       !utf8(r->local.task,sizeof r->local.task,32,1)||strlen(r->command_id)!=36||
       r->command_id[14]!='7'||!strchr("89ab",r->command_id[19]))return 0;
    for(unsigned i=0;i<65;i++){
        unsigned char c=(unsigned char)r->local.revision[i];
        if(i==64){if(c)return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }
    for(unsigned i=0;r->local.task[i];i++){
        unsigned char c=(unsigned char)r->local.task[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
    }
    for(unsigned i=0;i<36;i++){
        char c=r->command_id[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }
    return 1;
}
static int sync_valid(const br_result *r){return local_valid(r)&&r->origin==1;}
static int valid(const br_result *r){return r&&(r->origin?local_valid(r):br_result_valid(r));}
static int associated(br_inbox *q,const br_result *r){
    return r->origin?(q->sync_associate&&q->sync_associate(q->context,r)==1):q->associate(q->context,r)==1;
}
static int local_key(const br_result *a,const br_result *b){
    return a->local.serial==b->local.serial&&!strcmp(a->local.script,b->local.script)&&
        !strcmp(a->local.revision,b->local.revision)&&!strcmp(a->local.task,b->local.task);
}
static int command_key(const br_result *a,const br_result *b){return a->epoch==b->epoch&&!strcmp(a->command_id,b->command_id);}
static int key(const br_result *a,const br_result *b){
    if(a->origin!=b->origin)return 0;
    if(a->origin)return local_key(a,b);
    return !strcmp(a->task_id,b->task_id)&&!strcmp(a->command_id,b->command_id);
}
static int equal(const br_result *a,const br_result *b){return key(a,b)&&a->epoch==b->epoch&&
    !strcmp(a->command_id,b->command_id)&&
    !strcmp(a->app_version,b->app_version)&&!strcmp(a->hook_version,b->hook_version)&&!strcmp(a->text,b->text)&&!strcmp(a->code,b->code);}
static int pending(unsigned s){return s==BR_QUEUED||s==BR_PLAYING;}
static int policy_valid(unsigned p){return p<=BR_SPEECH_TIMEOUT;}
static int playable(const br_result *r,unsigned audible,unsigned policy){
    if(!audible)return 0;
    if(policy==BR_SPEECH_TIMEOUT)return 1;
    if(policy==BR_SPEECH_SCHEDULED)return r->text[0]||strcmp(r->code,"quota_exhausted");
    return !r->origin||r->text[0];
}
/* Explicit little-endian v1..v4 codec. v4 extends only enum values. */
typedef struct codec{unsigned char *p;size_t n,pos;int bad;} codec;
static void number(codec *c,uint64_t *v,unsigned bytes,int writing){
    if(c->bad||bytes>c->n-c->pos){c->bad=1;return;}
    if(!writing)*v=0;
    for(unsigned i=0;i<bytes;i++){
        if(writing)c->p[c->pos++]=(unsigned char)(*v>>(8*i));
        else *v|=(uint64_t)c->p[c->pos++]<<(8*i);
    }
}
static void string(codec *c,char *p,size_t cap,int writing){
    uint64_t n=writing?strlen(p):0;number(c,&n,2,writing);
    if(c->bad||n>=cap||n>c->n-c->pos){c->bad=1;return;}
    if(writing)memcpy(c->p+c->pos,p,(size_t)n);
    else {if(memchr(c->p+c->pos,0,(size_t)n)){c->bad=1;return;}memcpy(p,c->p+c->pos,(size_t)n);p[n]=0;}
    c->pos+=(size_t)n;
}
static void encode(codec *c,br_inbox *q,int writing){
    uint64_t version=q->schema,count=q->count;number(c,&version,4,writing);number(c,&count,4,writing);
    if(c->bad||version<1||version>4||count>BR_INBOX_ROWS){c->bad=1;return;}
    q->count=(unsigned)count;q->schema=(unsigned)version;
    for(unsigned i=0;i<q->count;i++){
        br_result_row *r=&q->rows[i];uint64_t state=r->state;
        uint64_t origin=r->result.origin;
        if(version>=2)number(c,&origin,1,writing);
        if(c->bad||origin>(version==4?2u:1u)){c->bad=1;return;}r->result.origin=(unsigned)origin;
        number(c,&state,1,writing);r->state=(unsigned)state;
        number(c,&r->received_ms,8,writing);number(c,&r->expires_ms,8,writing);number(c,&r->result.epoch,8,writing);
        if(origin){
            string(c,r->result.local.script,sizeof r->result.local.script,writing);
            string(c,r->result.local.revision,sizeof r->result.local.revision,writing);
            string(c,r->result.local.task,sizeof r->result.local.task,writing);
            number(c,&r->result.local.serial,8,writing);
            if(version==2){uint64_t mode=r->audible;number(c,&mode,1,writing);r->audible=(unsigned)mode;}
            string(c,r->result.command_id,sizeof r->result.command_id,writing);
        }else{
            if(version<3)r->audible=r->state!=BR_SILENT;
            string(c,r->result.task_id,sizeof r->result.task_id,writing);string(c,r->result.command_id,sizeof r->result.command_id,writing);
            string(c,r->result.app_version,sizeof r->result.app_version,writing);string(c,r->result.hook_version,sizeof r->result.hook_version,writing);
        }
        string(c,r->result.code,sizeof r->result.code,writing);string(c,r->result.text,sizeof r->result.text,writing);
        if(version>=3){
            uint64_t mode=r->audible,policy=r->speech_policy;
            number(c,&mode,1,writing);number(c,&policy,1,writing);
            r->audible=(unsigned)mode;r->speech_policy=(unsigned)policy;
        }
    }
}
static int save(br_inbox *q){
    unsigned char *bytes=malloc(BR_STORE_LIMIT);if(!bytes)return -1;
    codec c={.p=bytes,.n=BR_STORE_LIMIT};encode(&c,q,1);
    int rc=c.bad?-1:br_store_write(q->store,bytes,c.pos);free(bytes);return rc;
}
static int commit(br_inbox *q,br_inbox *next,int rc){
    if(save(next))rc=BR_IO;else *q=*next;
    free(next);return rc;
}
int br_inbox_open_sync(br_inbox *q,br_store *store,int (*associate)(void *,const br_result *),
                       int (*sync_associate)(void *,const br_result *),void *ctx){
    if(!q||!store||store->directory<0||!associate)return BR_INVALID;
    memset(q,0,sizeof *q);q->blocked=1;q->schema=1;q->store=store;q->associate=associate;q->context=ctx;q->sync_associate=sync_associate;
    unsigned char *bytes=malloc(BR_STORE_LIMIT);if(!bytes)return BR_IO;size_t length=0;
    int rc=br_store_read(store,bytes,BR_STORE_LIMIT,&length);
    if(rc<=0){free(bytes);if(rc==0)q->blocked=0;return rc==0?0:BR_IO;}
    codec c={.p=bytes,.n=length};encode(&c,q,0);free(bytes);
    unsigned active=0,playing=0,changed=0;
    if(c.bad||c.pos!=length)goto invalid;
    for(unsigned i=0;i<q->count;i++){
        br_result_row *r=&q->rows[i];
        if(!valid(&r->result)||(r->result.origin&&!sync_associate)||r->audible>1||!policy_valid(r->speech_policy)||
           ((r->result.origin==2)!=(r->speech_policy==BR_SPEECH_TIMEOUT))||
           (q->schema<4&&(r->state==BR_SUPPRESSED||r->speech_policy==BR_SPEECH_TIMEOUT))||
           (r->state==BR_SUPPRESSED?r->result.origin==2:
            ((r->state==BR_SILENT)!=!playable(&r->result,r->audible,r->speech_policy)))||
           r->state<BR_QUEUED||r->state>BR_SUPPRESSED||r->received_ms>UINT64_MAX-DAY||
           r->expires_ms<r->received_ms||r->expires_ms>r->received_ms+DAY)goto invalid;
        for(unsigned j=0;j<i;j++){
            br_result_row *other=&q->rows[j];
            if(key(&r->result,&other->result))goto invalid;
            if(r->result.origin==2||other->result.origin==2){
                if(r->result.origin&&other->result.origin&&local_key(&r->result,&other->result)&&
                   !command_key(&r->result,&other->result))goto invalid;
                if(command_key(&r->result,&other->result)){
                    if((r->result.origin&&other->result.origin&&!local_key(&r->result,&other->result))||
                       r->audible!=other->audible||
                       (r->result.origin==2?other->state:r->state)!=BR_SUPPRESSED)goto invalid;
                }
            }
        }
        active+=(unsigned)pending(r->state);playing+=r->state==BR_PLAYING;
        if(r->state==BR_PLAYING){r->state=BR_INTERRUPTED;changed=1;}
    }
    if(active>BR_INBOX_PENDING||playing>1)goto invalid;
    if(changed&&save(q)){q->count=0;return BR_IO;}q->blocked=0;return 0;
invalid:q->count=0;return BR_INVALID;
}
int br_inbox_open(br_inbox *q,br_store *store,int (*associate)(void *,const br_result *),void *ctx){
    return br_inbox_open_sync(q,store,associate,NULL,ctx);
}
static int receive(br_inbox *q,const br_result *r,uint64_t now,int trusted,int audible,unsigned ttl,enum br_speech_policy policy){
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!q->associate||!valid(r)||!boolean(trusted)||!boolean(audible)||!policy_valid((unsigned)policy)||
       ((r->origin==2)!=(policy==BR_SPEECH_TIMEOUT))||ttl<60||ttl>86400||
       now>UINT64_MAX-DAY||!associated(q,r))return BR_INVALID;
    if(!trusted)return BR_DEFER_CLOCK;
    br_inbox *next=malloc(sizeof *next);if(!next)return BR_IO;*next=*q;next->count=0;unsigned active=0;int suppressed=0;
    for(unsigned i=0;i<q->count;i++){
        br_result_row row=q->rows[i];
        if(!pending(row.state)&&now>=row.received_ms&&now-row.received_ms>=DAY)continue;
        if(key(&row.result,r)){int rc=equal(&row.result,r)&&(!r->origin||row.audible==(unsigned)audible)?BR_DUPLICATE:BR_INVALID;free(next);return rc;}
        if(r->origin==2||row.result.origin==2){
            /* A UUID/epoch identifies exactly one original scheduled command.
             * Refuse a local binding collision rather than silence another run. */
            if(r->origin&&row.result.origin&&local_key(&row.result,r)&&!command_key(&row.result,r)){free(next);return BR_INVALID;}
            if(command_key(&row.result,r)){
                if((r->origin&&row.result.origin&&!local_key(&row.result,r))||
                   !associated(q,&row.result)||row.audible!=(unsigned)audible){free(next);return BR_INVALID;}
                if(r->origin==2){free(next);return BR_ALREADY_TERMINAL;}
                suppressed=1;
            }
        }
        active+=(unsigned)pending(row.state);next->rows[next->count++]=row;
    }
    int play=!suppressed&&playable(r,(unsigned)audible,(unsigned)policy);
    if(next->count==BR_INBOX_ROWS||(play&&active>=BR_INBOX_PENDING)){free(next);return BR_DEFER_LEDGER;}
    next->rows[next->count++]=(br_result_row){.result=*r,.received_ms=now,.expires_ms=now+(uint64_t)ttl*1000,
        .state=suppressed?BR_SUPPRESSED:play?BR_QUEUED:BR_SILENT,.audible=(unsigned)audible,.speech_policy=(unsigned)policy};
    if(r->origin==2||suppressed)next->schema=4;
    else if(policy==BR_SPEECH_SCHEDULED&&next->schema<3)next->schema=3;
    else if(r->origin&&next->schema<2)next->schema=2;
    return commit(q,next,play?BR_ACCEPTED:BR_ACCEPTED_SILENT);
}
int br_inbox_receive(br_inbox *q,const br_result *r,int live,uint64_t connection,uint64_t now,int trusted,int audible,unsigned ttl){
    return br_inbox_receive_policy(q,r,live,connection,now,trusted,audible,ttl,BR_SPEECH_ORIGINAL);
}
int br_inbox_receive_policy(br_inbox *q,const br_result *r,int live,uint64_t connection,uint64_t now,int trusted,int audible,unsigned ttl,enum br_speech_policy policy){
    if(!br_result_valid(r)||!boolean(live)||!connection||(live&&r->epoch!=connection))return BR_INVALID;
    return receive(q,r,now,trusted,audible,ttl,policy);
}
int br_inbox_sync_receive(br_inbox *q,const br_result *r,uint64_t now,int trusted,int audible,unsigned ttl){
    return br_inbox_sync_receive_policy(q,r,now,trusted,audible,ttl,BR_SPEECH_ORIGINAL);
}
int br_inbox_sync_receive_policy(br_inbox *q,const br_result *r,uint64_t now,int trusted,int audible,unsigned ttl,enum br_speech_policy policy){
    if(!sync_valid(r))return BR_INVALID;
    return receive(q,r,now,trusted,audible,ttl,policy);
}
int br_inbox_timeout(br_inbox *q,const br_result *identity,uint64_t now,int trusted,int audible,unsigned ttl){
    if(!sync_valid(identity)||identity->code[0]||identity->text[0])return BR_INVALID;
    br_result *notice=malloc(sizeof(*notice));if(!notice)return BR_IO;
    *notice=*identity;notice->origin=2;
    int rc=receive(q,notice,now,trusted,audible,ttl,BR_SPEECH_TIMEOUT);
    memset(notice,0,sizeof(*notice));free(notice);return rc;
}
int br_inbox_sync_receipt(br_inbox *q,const br_result *r,int audible){
    const br_result *out=NULL;
    return br_inbox_sync_result(q,r,audible,&out);
}
int br_inbox_sync_authorized(br_inbox *q,const br_result *r){
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!sync_valid(r)||r->code[0]||r->text[0]||!associated(q,r))return BR_INVALID;
    return 1;
}
int br_inbox_sync_result(br_inbox *q,const br_result *r,int audible,const br_result **out){
    if(!out)return BR_INVALID;
    *out=NULL;
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!sync_valid(r)||r->code[0]||r->text[0]||!boolean(audible))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++){
        const br_result_row *row=&q->rows[i];
        if(!key(&row->result,r))continue;
        if(row->result.epoch!=r->epoch||strcmp(row->result.command_id,r->command_id)||
           row->audible!=(unsigned)audible||!associated(q,&row->result))return BR_INVALID;
        *out=&row->result;return 1;
    }
    return 0;
}
int br_inbox_ledger_result(br_inbox *q,const br_result *r,const br_result **out){
    if(!out)return BR_INVALID;
    *out=NULL;
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!q->associate||!br_result_valid(r)||r->code[0]||r->text[0]||q->count>BR_INBOX_ROWS)return BR_INVALID;
    if(!associated(q,r))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++){
        const br_result *saved=&q->rows[i].result;
        if(!key(saved,r))continue;
        if(!br_result_valid(saved)||saved->epoch!=r->epoch||strcmp(saved->app_version,r->app_version)||
           strcmp(saved->hook_version,r->hook_version)||!associated(q,saved))return BR_INVALID;
        *out=saved;return 1;
    }
    return 0;
}
int br_inbox_progress_read(br_inbox *q,const br_result *r,br_result_progress *out){
    if(!out)return BR_INVALID;
    if(q&&(q->blocked||!q->store||q->store->poisoned||q->store->directory<0||
           q->store->lock<0||q->store->schedule))return BR_IO;
    if(!q||!q->associate||!valid(r)||r->code[0]||r->text[0]||
       q->count>BR_INBOX_ROWS||q->schema<1||q->schema>4||!associated(q,r))return BR_INVALID;
    const br_result_row *found=NULL;
    for(unsigned i=0;i<q->count;i++){
        const br_result_row *row=&q->rows[i];const br_result *saved=&row->result;
        if(!valid(saved))return BR_INVALID;
        if(!key(saved,r))continue;
        if(found||saved->epoch!=r->epoch||strcmp(saved->command_id,r->command_id)||
           strcmp(saved->app_version,r->app_version)||strcmp(saved->hook_version,r->hook_version)||
           row->state<BR_QUEUED||row->state>BR_SUPPRESSED||row->audible>1||
           !policy_valid(row->speech_policy)||((saved->origin==2)!=(row->speech_policy==BR_SPEECH_TIMEOUT))||
           (q->schema<4&&(saved->origin==2||row->state==BR_SUPPRESSED))||
           (q->schema==1&&saved->origin)||
           (row->state==BR_SUPPRESSED?saved->origin==2:
            ((row->state==BR_SILENT)!=!playable(saved,row->audible,row->speech_policy)))||
           row->received_ms>UINT64_MAX-DAY||row->expires_ms<row->received_ms||
           row->expires_ms>row->received_ms+DAY||!associated(q,saved))return BR_INVALID;
        found=row;
    }
    if(!found)return 0;
    const br_result *saved=&found->result;
    br_result_progress v;memset(&v,0,sizeof v);
    v.epoch=saved->epoch;v.serial=saved->local.serial;
    v.received_ms=found->received_ms;v.expires_ms=found->expires_ms;
    v.origin=(uint8_t)saved->origin;v.state=(uint8_t)found->state;
    v.audible=(uint8_t)found->audible;v.speech_policy=(uint8_t)found->speech_policy;
    v.terminal=saved->origin!=2;
#define DISPLAY_COPY(field,source) memcpy(v.field,source,strlen(source))
    DISPLAY_COPY(command_id,saved->command_id);DISPLAY_COPY(task_id,saved->task_id);
    DISPLAY_COPY(app_version,saved->app_version);DISPLAY_COPY(hook_version,saved->hook_version);
    DISPLAY_COPY(script,saved->local.script);DISPLAY_COPY(revision,saved->local.revision);
    DISPLAY_COPY(task,saved->local.task);DISPLAY_COPY(code,saved->code);
#undef DISPLAY_COPY
    size_t n=strlen(saved->text);
    if(n>512){n=512;while(n&&((unsigned char)saved->text[n]&0xc0)==0x80)n--;v.truncated=1;}
    v.text_bytes=(uint16_t)n;memcpy(v.text,saved->text,n);
    memcpy(out,&v,sizeof v);return 1;
}
int br_inbox_take(br_inbox *q,uint64_t now,int trusted,int idle,br_result *out){
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!out||!boolean(trusted)||!boolean(idle))return BR_INVALID;
    if(!trusted||!idle)return 0;
    for(unsigned i=0;i<q->count;i++)if(q->rows[i].state==BR_PLAYING)return 0;
    br_inbox *next=malloc(sizeof *next);if(!next)return BR_IO;*next=*q;
    int chosen=-1,changed=0;
    for(unsigned i=0;i<next->count;i++){
        br_result_row *r=&next->rows[i];if(r->state!=BR_QUEUED)continue;
        /* Clock rollback never causes playback before receipt. */
        if(now<r->received_ms)continue;
        if(!associated(q,&r->result)){r->state=BR_FAILED;changed=1;}
        else if(now>=r->expires_ms){r->state=BR_EXPIRED;changed=1;}
        else if(chosen<0){chosen=(int)i;r->state=BR_PLAYING;changed=1;}
    }
    if(!changed){free(next);return 0;}
    br_result picked={0};if(chosen>=0)picked=next->rows[chosen].result;
    int rc=commit(q,next,chosen>=0?1:0);if(rc==1)*out=picked;return rc;
}
int br_inbox_playback_check(br_inbox *q,const br_result *r,uint64_t now,int trusted){
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!valid(r)||!boolean(trusted))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++)if(q->rows[i].state==BR_PLAYING&&equal(&q->rows[i].result,r)){
        const br_result_row *row=&q->rows[i];
        if(!associated(q,r))return BR_PLAY_REVOKED;
        if(!trusted||now<row->received_ms)return BR_PLAY_WAIT;
        return now>=row->expires_ms?BR_PLAY_EXPIRED:BR_PLAY_READY;
    }
    return BR_INVALID;
}
int br_inbox_speech(br_inbox *q,const br_result *r,const char **out){
    if(!out)return BR_INVALID;
    *out=NULL;
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!valid(r))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++){
        const br_result_row *row=&q->rows[i];
        if(row->state!=BR_PLAYING||!equal(&row->result,r))continue;
        if(!associated(q,r)||!row->audible||!policy_valid(row->speech_policy))return BR_INVALID;
        *out=row->result.text;
        if(row->speech_policy==BR_SPEECH_TIMEOUT)*out="定时任务执行超时，结果尚未确认";
        else if(!**out&&row->speech_policy==BR_SPEECH_SCHEDULED&&strcmp(r->code,"quota_exhausted"))
            *out=r->code[0]?"定时任务执行失败":"定时任务已完成";
        return 1;
    }
    return BR_INVALID;
}
int br_inbox_finish(br_inbox *q,const br_result *r,enum br_result_state status){
    if(q&&(q->blocked||!q->store||q->store->poisoned))return BR_IO;
    if(!q||!valid(r)||(status!=BR_DONE&&status!=BR_FAILED&&status!=BR_INTERRUPTED&&status!=BR_EXPIRED))return BR_INVALID;
    for(unsigned i=0;i<q->count;i++)if(q->rows[i].state==BR_PLAYING&&equal(&q->rows[i].result,r)){
        br_inbox *next=malloc(sizeof *next);if(!next)return BR_IO;*next=*q;next->rows[i].state=(unsigned)status;return commit(q,next,0);
    }
    return BR_INVALID;
}
