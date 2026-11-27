#include "flow_owner_handoff.h"
#include <string.h>

#define HEADER 460u
#define STEP 46u
#define VERSIONS 387u
_Static_assert(HEADER+(STEP+VERSIONS)*BR_FLOW_STEPS==BR_FLOW_OWNER_HANDOFF_BYTES,
    "complete bounded original-owner image");
static uint64_t number(unsigned char *p,uint64_t v,unsigned n,int writing){
    if(writing)for(unsigned i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i));
    else{v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);}
    return v;
}
/* Canonical zero padding is part of the format, never host struct padding. */
static int fixed(const char *s,size_t cap,int hash){
    size_t n=0;
    for(;n<cap&&s[n];n++){
        unsigned c=(unsigned char)s[n];
        if(hash?!((c>='0'&&c<='9')||(c>='a'&&c<='f')):
           !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
    }
    if(!n||n==cap||(hash&&n!=64))return 0;
    while(++n<cap)if(s[n])return 0;
    return 1;
}
static int valid_binding(const br_flow_session_log_binding *b){
    return b&&fixed(b->script,sizeof b->script,0)&&fixed(b->flow_sha256,65,1)&&
        fixed(b->revision,65,1)&&fixed(b->definition_sha256,65,1)&&fixed(b->inputs_sha256,65,1)&&
        b->run&&b->session_run&&b->epoch&&b->generation&&
        ((!b->ctl_run&&!b->cycle&&!b->job)||(b->ctl_run&&b->cycle&&b->job))&&
        b->count&&b->count<=BR_FLOW_STEPS&&b->source>=BR_LOG_OFFICIAL&&b->source<=BR_LOG_SCHEDULED;
}
static int binding_image(unsigned char *p,const br_flow_session_log_binding *b,int writing){
    const char *strings[]={b->script,b->flow_sha256,b->revision,b->definition_sha256,b->inputs_sha256};
    const size_t sizes[]={129,65,65,65,65};
    for(unsigned i=0;i<5;i++){
        if(writing)memcpy(p,strings[i],sizes[i]);else if(memcmp(p,strings[i],sizes[i]))return 0;
        p+=sizes[i];
    }
    const uint64_t values[]={b->run,b->session_run,b->epoch,b->generation,b->ctl_run,b->cycle,b->job,b->count,b->source};
    for(unsigned i=0;i<9;i++){
        unsigned n=i<7?8:1;
        if(number(p,values[i],n,writing)!=values[i])return 0;
        p+=n;
    }
    return 1;
}
static int uuid(const char *s){
    if(s[36]||s[14]!='7'||!(s[19]=='8'||s[19]=='9'||s[19]=='a'||s[19]=='b'))return 0;
    for(unsigned i=0;i<36;i++){
        unsigned c=(unsigned char)s[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return 0;
    }
    return 1;
}
/* Fixed129 byte identity field: same UTF-8/control limits as K6, plus zero
 * padding. At most128 bytes already bounds codepoints to at most128. */
static int version(const unsigned char *p){
    size_t n=0;while(n<129&&p[n])n++;
    if(!n||n==129)return 0;
    for(size_t i=n+1;i<129;i++)if(p[i])return 0;
    for(size_t i=0;i<n;){
        uint32_t x=p[i++],min=0;unsigned rest=0;
        if(x<32||x==127)return 0;
        if(x<128)continue;
        if(x>=0xc2&&x<=0xdf){rest=1;min=0x80;x&=31;}
        else if(x>=0xe0&&x<=0xef){rest=2;min=0x800;x&=15;}
        else if(x>=0xf0&&x<=0xf4){rest=3;min=0x10000;x&=7;}
        else return 0;
        if(rest>n-i)return 0;
        while(rest--){unsigned c=p[i++];if((c&0xc0)!=0x80)return 0;x=(x<<6)|(c&63);}
        if(x<min||x>0x10ffff||(x>=0xd800&&x<=0xdfff))return 0;
    }
    return 1;
}
static int valid_step(const br_flow_session_log_step *steps,unsigned i){
    const br_flow_session_log_step *s=steps+i;
    if(!s->ticket||!uuid(s->command)||(i&&s->ticket<=steps[i-1].ticket))return 0;
    for(unsigned k=0;k<i;k++)if(!strcmp(s->command,steps[k].command))return 0;
    return 1;
}
static int valid_row(const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,
    const br_result *const *ids,unsigned i){
    const br_flow_session_log_step *s=steps+i;const br_result *r=ids[i];
    if(!valid_step(steps,i))return 0;
    return !r||(br_result_valid(r)&&!r->code[0]&&!r->text[0]&&r->epoch==b->epoch&&
        !strcmp(r->command_id,s->command));
}
int br_flow_owner_handoff_encode(const br_flow_session_log_binding *b,
    const br_flow_session_log_step *steps,const br_result *const *ids,unsigned count,
    void *output,size_t capacity,size_t *length){
    if(length)*length=0;
    if(!output||capacity!=BR_FLOW_OWNER_HANDOFF_BYTES)return BR_INVALID;
    memset(output,0,capacity);
    if(!length||!valid_binding(b)||count>b->count||(count&&(!steps||!ids)))return BR_INVALID;
    unsigned char *p=output;size_t n=HEADER;
    memcpy(p,"LX06OWN1",8);(void)number(p+8,1,4,1);(void)binding_image(p+12,b,1);p[459]=(unsigned char)count;
    for(unsigned i=0;i<count;i++){
        const br_flow_session_log_step *s=steps+i;const br_result *r=ids[i];
        if(!valid_row(b,steps,ids,i))goto bad;
        (void)number(p+n,s->ticket,8,1);memcpy(p+n+8,s->command,37);p[n+45]=(unsigned char)(r!=NULL);n+=STEP;
        if(r){
            memcpy(p+n,r->task_id,strlen(r->task_id));
            memcpy(p+n+129,r->app_version,strlen(r->app_version));
            memcpy(p+n+258,r->hook_version,strlen(r->hook_version));n+=VERSIONS;
        }
    }
    *length=n;return 1;
bad:memset(output,0,capacity);return BR_INVALID;
}
static int scan(const void *bytes,size_t length,const br_flow_session_log_binding *b,
    unsigned index,const br_flow_session_log_step *step,const br_result *r){
    if(!bytes||length<HEADER||length>BR_FLOW_OWNER_HANDOFF_BYTES)return 0;
    /* number/binding_image do not write when writing=0. No alignment casts. */
    unsigned char *p=(unsigned char *)bytes;
    if(memcmp(p,"LX06OWN1",8)||number(p+8,0,4,0)!=1||!fixed((char *)p+12,129,0))return 0;
    for(unsigned i=0;i<4;i++)if(!fixed((char *)p+141+i*65,65,1)||!number(p+401+i*8,0,8,0))return 0;
    uint64_t ctl=number(p+433,0,8,0),cycle=number(p+441,0,8,0),job=number(p+449,0,8,0);
    if(!((!ctl&&!cycle&&!job)||(ctl&&cycle&&job))||!p[457]||p[457]>BR_FLOW_STEPS||
       p[458]<BR_LOG_OFFICIAL||p[458]>BR_LOG_SCHEDULED)return 0;
    unsigned count=p[459];if(count>p[457])return 0;
    if(b&&(!valid_binding(b)||!step||index>=count||!br_result_valid(r)||r->code[0]||r->text[0]||
        r->epoch!=b->epoch||!binding_image(p+12,b,0)))return 0;
    const unsigned char *commands[BR_FLOW_STEPS];uint64_t prior=0;size_t n=HEADER;int found=!b;
    for(unsigned i=0;i<count;i++){
        if(STEP>length-n)return 0;
        uint64_t ticket=number(p+n,0,8,0);const unsigned char *command=p+n+8;unsigned known=p[n+45];
        if(ticket<=prior||!uuid((const char *)command)||known>1)return 0;
        for(unsigned k=0;k<i;k++)if(!memcmp(commands[k],command,37))return 0;
        commands[i]=command;prior=ticket;n+=STEP;
        if(known){
            if(VERSIONS>length-n)return 0;
            for(unsigned k=0;k<3;k++)if(!version(p+n+k*129))return 0;
            if(b&&i==index)found=ticket==step->ticket&&!memcmp(command,step->command,37)&&
                !strcmp((const char *)command,r->command_id)&&!strcmp((const char *)p+n,r->task_id)&&
                !strcmp((const char *)p+n+129,r->app_version)&&!strcmp((const char *)p+n+258,r->hook_version);
            n+=VERSIONS;
        }
    }
    return n==length&&found;
}
int br_flow_owner_handoff_validate(const void *bytes,size_t length){
    return scan(bytes,length,NULL,0,NULL,NULL)?1:BR_INVALID;
}
int br_flow_owner_handoff_prefix(const void *bytes,size_t length,
    const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,
    unsigned count,unsigned *published){
    if(!published)return BR_INVALID;
    *published=0;
    if(!valid_binding(b)||count>b->count||(count&&!steps)||
       !scan(bytes,length,NULL,0,NULL,NULL))return BR_INVALID;
    for(unsigned i=0;i<count;i++)if(!valid_step(steps,i))return BR_INVALID;
    unsigned char *p=(unsigned char *)bytes;
    if(!binding_image(p+12,b,0))return BR_INVALID;
    unsigned common=count<p[459]?count:p[459];size_t n=HEADER;
    for(unsigned i=0;i<common;i++){
        if(number(p+n,0,8,0)!=steps[i].ticket||memcmp(p+n+8,steps[i].command,37))return BR_INVALID;
        n+=STEP+(p[n+45]?VERSIONS:0);
    }
    *published=p[459];return 1;
}
int br_flow_owner_handoff_append_prepare(void *bytes,size_t capacity,size_t *length,
    const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,unsigned count){
    unsigned published=0;
    if(capacity!=BR_FLOW_OWNER_HANDOFF_BYTES||!length||!count||
       br_flow_owner_handoff_prefix(bytes,*length,b,steps,count,&published)!=1||
       count!=published+1||*length>capacity-STEP)return BR_INVALID;
    unsigned char *p=bytes;size_t n=*length;
    (void)number(p+n,steps[count-1].ticket,8,1);
    memcpy(p+n+8,steps[count-1].command,37);p[n+45]=0;
    p[459]=(unsigned char)count;*length=n+STEP;return 1;
}
int br_flow_owner_handoff_transition(const void *bytes,size_t length,unsigned event,
    const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,
    const br_result *const *ids,unsigned count){
    if(!valid_binding(b)||count>b->count||(count&&(!steps||!ids)))return BR_INVALID;
    for(unsigned i=0;i<count;i++)if(!valid_row(b,steps,ids,i))return BR_INVALID;
    if(event==BR_OWNER_INIT)return !bytes&&!length&&!count?1:BR_INVALID;
    if((event!=BR_OWNER_PREPARE&&event!=BR_OWNER_ASSOCIATE)||
       !scan(bytes,length,NULL,0,NULL,NULL))return BR_INVALID;
    unsigned char *p=(unsigned char *)bytes;
    unsigned old_count=p[459],changes=0;size_t n=HEADER;
    if(!binding_image(p+12,b,0)||
       (event==BR_OWNER_PREPARE?(count!=old_count+1||ids[count-1]!=NULL):count!=old_count))return BR_INVALID;
    for(unsigned i=0;i<old_count;i++){
        const br_result *r=ids[i];unsigned known=p[n+45];
        if(number(p+n,0,8,0)!=steps[i].ticket||memcmp(p+n+8,steps[i].command,37))return BR_INVALID;
        n+=STEP;
        if(known){
            if(!r||strcmp((char *)p+n,r->task_id)||strcmp((char *)p+n+129,r->app_version)||
               strcmp((char *)p+n+258,r->hook_version))return BR_INVALID;
            n+=VERSIONS;
        }else if(r)changes++;
    }
    return changes==(event==BR_OWNER_ASSOCIATE?1u:0u)?1:BR_INVALID;
}
int br_flow_owner_handoff_associate_step(void *bytes,size_t capacity,size_t *length,
    const br_flow_session_log_binding *b,const br_flow_session_log_step *steps,unsigned count,
    unsigned index,const br_result *r){
    unsigned published=0;
    if(capacity!=BR_FLOW_OWNER_HANDOFF_BYTES||!length||index>=count||
       br_flow_owner_handoff_prefix(bytes,*length,b,steps,count,&published)!=1||published!=count||
       !br_result_valid(r)||r->code[0]||r->text[0]||r->epoch!=b->epoch||
       strcmp(r->command_id,steps[index].command)||*length>capacity-VERSIONS)return BR_INVALID;
    unsigned char *p=bytes;size_t n=HEADER;
    for(unsigned i=0;i<index;i++)n+=STEP+(p[n+45]?VERSIONS:0);
    if(p[n+45])return BR_INVALID;
    size_t at=n+STEP;memmove(p+at+VERSIONS,p+at,*length-at);memset(p+at,0,VERSIONS);
    memcpy(p+at,r->task_id,strlen(r->task_id));memcpy(p+at+129,r->app_version,strlen(r->app_version));
    memcpy(p+at+258,r->hook_version,strlen(r->hook_version));p[n+45]=1;*length+=VERSIONS;return 1;
}
int br_flow_owner_handoff_associate(void *p,const br_flow_session_log_binding *b,
    unsigned index,const br_flow_session_log_step *step,const br_result *r){
    br_flow_owner_handoff *v=p;
    if(!v||v->busy||!v->current)return BR_INVALID;
    v->busy=1;int rc=BR_INVALID;
    if(b&&scan(v->bytes,v->length,b,index,step,r)&&v->current(v->context,v->bytes,v->length)==1)rc=1;
    v->busy=0;return rc;
}
