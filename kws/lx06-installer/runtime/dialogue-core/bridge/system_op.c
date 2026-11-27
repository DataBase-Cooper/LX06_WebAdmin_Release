#include "system_op.h"
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(br_system)<12*1024,"bounded local executor state");
/* Strict UTF-8 scalar sequence, no ASCII control characters. Bounds also
 * apply to typed owner strings, not only decoded JSON strings. */
static size_t text_count(const unsigned char *p,size_t n){
    size_t count=0;
    for(size_t i=0;i<n;count++){
        unsigned c=p[i++],more=0,value=c,min=0;
        if(c<32||c==127)return 0;
        if(c>=128){
            if(c>=0xc2&&c<=0xdf){more=1;value=c&31;min=128;}
            else if(c>=0xe0&&c<=0xef){more=2;value=c&15;min=2048;}
            else if(c>=0xf0&&c<=0xf4){more=3;value=c&7;min=65536;}
            else return 0;
            if(more>n-i)return 0;
            while(more--){c=p[i++];if((c&0xc0)!=0x80)return 0;value=(value<<6)|(c&63);}
            if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
        }
    }
    return count;
}
static int name_valid(const char *s){
    if(!s)return 0;
    size_t n=0;while(n<=128&&s[n])n++;
    return n&&n<=128&&text_count((const unsigned char *)s,n);
}
static int active(const br_system *s){
    return s->status==BR_SYSTEM_QUEUED||s->status==BR_SYSTEM_RUNNING||s->status==BR_SYSTEM_DRAINING;
}
static int scratch_ok(const br_system *s,const br_json *r){
    return r&&(!s->caps||r!=&s->caps->document);
}
static int rules_valid(const br_system_rule *rules,size_t count){
    if(!rules||!count||count>32)return 0;
    for(size_t i=0;i<count;i++){
        const br_system_rule *r=rules+i;
        if(!name_valid(r->op)||r->kind<BR_SYSTEM_TTS||r->kind>BR_SYSTEM_ACK||
           !r->timeout_ms||r->timeout_ms>120000)return 0;
        for(size_t j=0;j<i;j++)if(!strcmp(r->op,rules[j].op))return 0;
        if(r->kind==BR_SYSTEM_FILE){
            if(r->root<0||!r->paths||!r->path_count||r->path_count>32)return 0;
            for(size_t k=0;k<r->path_count;k++){
                if(!br_system_path_valid(r->paths[k]))return 0;
                for(size_t j=0;j<k;j++)if(!strcmp(r->paths[j],r->paths[k]))return 0;
            }
        }
    }
    return 1;
}
static void finish(br_system *s,enum br_system_status terminal){
    if(s->fd>=0)close(s->fd);
    s->fd=-1;s->status=terminal;
    memset(s->text,0,sizeof(s->text));memset(s->request,0,sizeof(s->request));
    s->request_length=0;s->caps=NULL;
}
static void stop(br_system *s,enum br_system_status terminal,const char *error,uint64_t now){
    if(s->status==BR_SYSTEM_DRAINING)return; /* preserve first terminal cause */
    s->error=error;s->terminal=terminal;
    if(s->status==BR_SYSTEM_QUEUED){finish(s,terminal);return;}
    s->status=BR_SYSTEM_DRAINING;s->io.cancel(s->io.context,s->ticket,now);
}
int br_system_init(br_system *s,br_system_io io){
    if(!s||!io.start_tts||!io.start_file||!io.poll||!io.cancel)return -1;
    memset(s,0,sizeof(*s));s->io=io;s->fd=-1;return 1;
}
int br_system_submit(br_system *s,const br_caps *caps,uint64_t epoch,uint64_t ticket,
                     uint64_t now,const void *data,size_t length,const br_system_rule *rules,size_t count,br_json *r){
    if(!s||!scratch_ok(s,r)||(caps&&r==&caps->document))return -1;
    int rc=-1,fd=-1;
    if(active(s)){memset(r,0,sizeof(*r));return 0;}
    if(!s->io.start_tts||!s->io.start_file||!s->io.poll||!s->io.cancel||!ticket||ticket<=s->last_ticket||
       (s->last_ticket&&now<s->now_ms)||
       !rules_valid(rules,count)||br_caps_system_request(caps,epoch,data,length,r)!=BR_CAPS_OK)goto done;
    /* The shared admission scratch is deliberately erased. Decode once more
     * with the SAME shared parser for copying the admitted local arguments. */
    if(br_json_parse(r,data,length)!=1)goto done;
    unsigned request=br_json_field(r,1,"params"),name=br_json_field(r,request,"op");
    const br_system_rule *rule=NULL;
    for(size_t i=0;i<count;i++)if(br_json_is(r,name,rules[i].op)){rule=rules+i;break;}
    if(!rule)goto done;
    unsigned params=br_json_field(r,request,"params"),arg=0;
    const char *key=rule->kind==BR_SYSTEM_TTS?"text":"path";
    for(unsigned k=br_json_child(r,params);k&&k<br_json_next(r,params);k=br_json_next(r,k+1)){
        if(rule->kind==BR_SYSTEM_ACK||!br_json_is(r,k,key))goto done;
        arg=k+1;
    }
    br_text value=br_json_text(r,arg);
    if(rule->kind!=BR_SYSTEM_ACK){
        if(br_json_kind(r,arg)!=BR_J_STRING||!value.length)goto done;
        size_t chars=text_count(value.data,value.length);
        if(!chars||(rule->kind==BR_SYSTEM_TTS&&(value.length>2048||chars>512)))goto done;
        if(rule->kind==BR_SYSTEM_FILE){
            char path[BR_SYSTEM_PATH_MAX+1];
            if(value.length>BR_SYSTEM_PATH_MAX)goto done;
            memcpy(path,value.data,value.length);path[value.length]=0;
            fd=br_system_file_open(rule->root,path,rule->paths,rule->path_count);
            if(fd<0)goto done;
        }
    }
    s->caps=caps;s->epoch=epoch;s->ticket=s->last_ticket=ticket;s->started_ms=s->now_ms=now;
    s->timeout_ms=rule->timeout_ms;s->kind=rule->kind;s->error=NULL;s->terminal=BR_SYSTEM_IDLE;
    s->fd=fd;fd=-1;s->status=BR_SYSTEM_QUEUED;
    memcpy(s->request,data,length);s->request_length=length;
    if(rule->kind==BR_SYSTEM_TTS){memcpy(s->text,value.data,value.length);s->text[value.length]=0;}
    if(rule->kind==BR_SYSTEM_ACK)finish(s,BR_SYSTEM_DONE);
    rc=1;
done:
    if(fd>=0)close(fd);
    memset(r,0,sizeof(*r));return rc;
}
int br_system_poll(br_system *s,uint64_t ticket,uint64_t now,int preempt,br_json *r){
    if(!s||!scratch_ok(s,r))return -1;
    memset(r,0,sizeof(*r));
    if(!ticket||ticket!=s->ticket)return -1; /* stale must not alter clock */
    if(preempt!=0&&preempt!=1)return -1;
    if(!active(s))return s->status;
    if(now<s->now_ms){now=s->now_ms;stop(s,BR_SYSTEM_FAILED,"bridge_unreachable",now);}
    s->now_ms=now;
    if(s->status!=BR_SYSTEM_DRAINING){
        if(!s->caps||!s->caps->ready||s->caps->connection!=s->epoch)
            stop(s,BR_SYSTEM_FAILED,"bridge_unreachable",now);
        else if(preempt)stop(s,BR_SYSTEM_CANCELLED,NULL,now);
        else if(now-s->started_ms>=s->timeout_ms)stop(s,BR_SYSTEM_FAILED,"op_timeout",now);
    }
    if(s->status==BR_SYSTEM_QUEUED){
        int rc=br_caps_system_request(s->caps,s->epoch,s->request,s->request_length,r);
        if(rc!=BR_CAPS_OK)stop(s,BR_SYSTEM_FAILED,br_caps_error(rc),now);
        else{
            uint64_t remaining=s->timeout_ms-(now-s->started_ms);
            int started=s->kind==BR_SYSTEM_TTS?
                s->io.start_tts(s->io.context,s->ticket,s->text,now,remaining):
                s->io.start_file(s->io.context,s->ticket,s->fd,now,remaining);
            if(started){s->status=BR_SYSTEM_RUNNING;if(started!=1)stop(s,BR_SYSTEM_FAILED,"unsupported",now);}
        }
    }
    if(s->status==BR_SYSTEM_RUNNING||s->status==BR_SYSTEM_DRAINING){
        enum br_system_io_status io=s->io.poll(s->io.context,s->ticket,now);
        if(io==BR_SYSTEM_IO_DONE||io==BR_SYSTEM_IO_FAILED||io==BR_SYSTEM_IO_CANCELLED){
            if(s->status==BR_SYSTEM_DRAINING)finish(s,s->terminal);
            else if(io==BR_SYSTEM_IO_DONE)finish(s,BR_SYSTEM_DONE);
            else if(io==BR_SYSTEM_IO_CANCELLED)finish(s,BR_SYSTEM_CANCELLED);
            else{s->error="unsupported";finish(s,BR_SYSTEM_FAILED);}
        }else if(io!=BR_SYSTEM_IO_RUNNING)stop(s,BR_SYSTEM_FAILED,"unsupported",now);
    }
    return s->status;
}
int br_system_cancel(br_system *s,uint64_t ticket,uint64_t now,br_json *r){
    return br_system_poll(s,ticket,now,1,r);
}
