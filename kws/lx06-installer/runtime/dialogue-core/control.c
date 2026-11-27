#define _POSIX_C_SOURCE 200809L
#include "control.h"
#include "../doubao/src/text_match.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

static void decision(dc_control *c, dc_decision *d) {
    memset(d,0,sizeof *d);d->current=c->tag;d->state=c->state;
}
static int clean_words(const char *s) {
    return memchr(s,0,DC_WORDS_CAP)!=NULL;
}
int dc_config_valid(const dc_config *config) {
    if(!config||!clean_words(config->interrupt)||!clean_words(config->goodbye)||
       config->barge_any>1||(config->idle_ms&&config->idle_ms<1000)||config->idle_ms>86400000||
       config->tail_ms>5000||!isfinite(config->gain_db)||config->gain_db < -24||config->gain_db>24) return 0;
    /* All accepted lists must be usable by both stripping and matching; the
     * latter otherwise silently truncates tokens at 127 bytes. */
    char empty[1];
    return !dc_control_strip(empty,sizeof empty,"",config->interrupt)&&
           !dc_control_strip(empty,sizeof empty,"",config->goodbye);
}
int dc_control_open(dc_control *c,uint64_t epoch,const dc_config *config,uint64_t now) {
    if(!c||!epoch||!dc_config_valid(config))return -1;
    memset(c,0,sizeof *c);c->tag.session_epoch=epoch;c->config=*config;
    c->state=DC_CONNECTING;c->last_activity_ms=now;return 0;
}
dc_tag dc_control_reply(dc_control *c,uint64_t utterance,uint64_t now) {
    dc_tag invalid={0};
    if(!c||c->closed||c->reply_live||!utterance||c->next_generation>=UINT64_MAX-1) return invalid;
    c->tag.reply_generation=++c->next_generation;c->tag.utterance_id=utterance;
    c->reply_live=1;c->state=DC_THINKING;c->last_activity_ms=now;return c->tag;
}
int dc_control_state(dc_control *c,dc_tag tag,enum dc_state state,uint64_t now) {
    if(!c||c->closed||!dc_tag_equal(c->tag,tag)||state<=DC_CLOSED||state>=DC_CLOSING) return -1;
    c->state=state;c->last_activity_ms=now;return 0;
}
static int cancel_reply(dc_control *c,dc_tag source,uint64_t now,dc_decision *d) {
    if(!c->reply_live||source.reply_generation!=c->tag.reply_generation||
       c->last_cancelled==c->tag.reply_generation) return 0;
    d->cancelled=c->tag;c->last_cancelled=c->tag.reply_generation;
    c->interrupted_utterance=source.utterance_id;c->reply_live=0;
    c->tail_until_ms=now+c->config.tail_ms;c->last_activity_ms=now;
    c->state=DC_LISTENING;
    d->actions|=DC_ACT_STOP|DC_ACT_CANCEL|DC_ACT_STATE;d->state=c->state;
    return 1;
}
int dc_control_close(dc_control *c,uint64_t now,dc_decision *d) {
    if(!c||!d) return -1;
    decision(c,d);if(c->closed) return 0;
    if(c->reply_live){d->cancelled=c->tag;d->actions|=DC_ACT_CANCEL;}
    d->actions|=DC_ACT_STOP|DC_ACT_END|DC_ACT_STATE;c->reply_live=0;c->closed=1;
    c->state=DC_CLOSING;c->last_activity_ms=now;d->state=c->state;return 1;
}
int dc_control_finish_close(dc_control *c,uint64_t now,dc_decision *d) {
    if(!c||!d)return -1;
    decision(c,d);
    if(c->state==DC_CLOSED)return 0;
    if(!c->closed||c->state!=DC_CLOSING)return -1;
    c->state=DC_CLOSED;c->last_activity_ms=now;
    d->actions=DC_ACT_STATE;d->state=DC_CLOSED;return 1;
}
/* Normalize with byte spans to strip phrases without discarding a following
 * question. Reject malformed UTF-8 rather than searching through replacement
 * characters. The bounded input is never a logging destination. */
struct point {uint32_t value;uint16_t begin,end;};
static int ignored(unsigned cp) {
    if(cp<128) return isspace((unsigned char)cp)||ispunct((unsigned char)cp)||cp<32;
    return cp==0x3000||cp==0x3001||cp==0x3002||cp==0xff0c||cp==0xff1b||
           cp==0xff01||cp==0xff1f||cp==0xff1a||cp==0x2018||cp==0x2019||cp==0x201c||cp==0x201d;
}
static int points(const char *s,struct point *p,size_t cap,size_t *count) {
    size_t i=0,n=0,len=strlen(s);
    while(i<len){
        size_t begin=i,k;unsigned cp=(unsigned char)s[i++];
        if(cp<128)k=0;else if(cp>=0xc2&&cp<=0xdf){cp&=31;k=1;}
        else if(cp>=0xe0&&cp<=0xef){cp&=15;k=2;}else if(cp>=0xf0&&cp<=0xf4){cp&=7;k=3;}else return -1;
        if(i+k>len)return -1;
        for(size_t j=0;j<k;j++){unsigned b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return -1;cp=(cp<<6)|(b&63);}
        if((k==1&&cp<128)||(k==2&&cp<2048)||(k==3&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return -1;
        if(cp<128)cp=(unsigned)tolower((unsigned char)cp);
        if(!ignored(cp)){if(n==cap)return -1;p[n++]=(struct point){cp,begin,i};}
    }
    *count=n;return 0;
}
static size_t separator(const char *p) {
    if(*p==','||*p==';'||*p=='\r'||*p=='\n')return 1;
    return !strncmp(p,"，",3)||!strncmp(p,"、",3)?3:0;
}
int dc_control_strip(char *out,size_t cap,const char *text,const char *words) {
    if(!out||!cap||!text||!words)return -1;
    size_t len=strnlen(text,DC_QUESTION_CAP);if(len==DC_QUESTION_CAP||len+1>cap)return -1;
    struct point observed[DC_QUESTION_CAP],phrase[128];size_t n;
    unsigned char remove[DC_QUESTION_CAP]={0};
    if(points(text,observed,DC_QUESTION_CAP,&n))return -1;
    const char *at=words;
    while(*at){
        while(*at&&(separator(at)||isspace((unsigned char)*at)))at+=separator(at)?separator(at):1;
        const char *end=at;while(*end&&!separator(end))end++;
        size_t bytes=(size_t)(end-at);if(bytes>=128)return -1;
        char token[128];memcpy(token,at,bytes);token[bytes]=0;size_t count;
        if(points(token,phrase,128,&count))return -1;
        if(count>=2&&count<=n)for(size_t i=0;i+count<=n;i++){
            size_t j=0;while(j<count&&observed[i+j].value==phrase[j].value)j++;
            if(j==count)for(size_t b=observed[i].begin;b<observed[i+count-1].end;b++)remove[b]=1;
        }
        at=end;
    }
    size_t used=0;for(size_t i=0;i<len;i++)if(!remove[i])out[used++]=text[i];out[used]=0;
    /* Trim punctuation only at the resulting boundary, retaining punctuation
     * and original spelling inside the actual following question. */
    size_t left;
    if(points(out,observed,DC_QUESTION_CAP,&left))return -1;
    if(!left){out[0]=0;return 0;}
    size_t begin=observed[0].begin,end=observed[left-1].end;
    memmove(out,out+begin,end-begin);out[end-begin]=0;return 0;
}
int dc_control_text(dc_control *c,dc_tag source,const char *text,int final,int echo,uint64_t now,dc_decision *d) {
    if(!c||!text||!d||!source.utterance_id||strnlen(text,DC_QUESTION_CAP)==DC_QUESTION_CAP)return -1;
    decision(c,d);if(c->closed||source.session_epoch!=c->tag.session_epoch||echo||
                     source.utterance_id<=c->last_final_utterance)return 0;
    int current=source.reply_generation==c->tag.reply_generation;
    int interrupted=!c->reply_live&&source.reply_generation==c->last_cancelled&&
                    source.utterance_id==c->interrupted_utterance&&c->last_cancelled;
    /* A completed utterance from another listening generation is stale. */
    if(!current&&!interrupted)return 0;
    /* Validate before matching controls: neither malformed text nor an
     * unstrippable final may consume its serial or cancel a live reply. */
    char question[DC_QUESTION_CAP];
    if(final){
        if(dc_control_strip(question,sizeof question,text,c->config.interrupt))return -1;
    }else{
        struct point parsed[DC_QUESTION_CAP];size_t count;
        if(points(text,parsed,DC_QUESTION_CAP,&count))return -1;
    }
    int bye=xz_text_matches_list(text,c->config.goodbye);
    int barge=xz_text_matches_list(text,c->config.interrupt);
    if(bye)return dc_control_close(c,now,d);
    d->interrupt_matched=current&&barge;
    if(barge)cancel_reply(c,source,now,d);
    if(final&&source.utterance_id>c->last_final_utterance){
        c->last_final_utterance=source.utterance_id;
        memcpy(d->question,question,strlen(question)+1);
        if(d->question[0]){
            /* Normal speech while answering is ignored in strict word mode;
             * speech after an accepted control keeps its original utterance. */
            if(!c->reply_live||barge||c->config.barge_any){
                if(c->reply_live)cancel_reply(c,source,now,d);
                d->actions|=DC_ACT_QUESTION;c->last_activity_ms=now;
            }else d->question[0]=0;
        }
    }
    d->current=c->tag;d->state=c->state;return d->actions?1:0;
}
int dc_control_voice(dc_control *c,dc_tag source,int near_end,int echo,uint64_t now,dc_decision *d) {
    if(!c||!d)return -1;
    decision(c,d);
    if(c->closed||!c->config.barge_any||!near_end||echo||now<c->tail_until_ms||
       source.session_epoch!=c->tag.session_epoch||!source.utterance_id)return 0;
    return cancel_reply(c,source,now,d);
}
int dc_control_finish_reply(dc_control *c,dc_tag tag,uint64_t now) {
    if(!c||c->closed||!dc_tag_equal(c->tag,tag)||!c->reply_live)return -1;
    c->reply_live=0;c->state=DC_LISTENING;c->tail_until_ms=now+c->config.tail_ms;
    c->last_activity_ms=now;return 0;
}
int dc_control_tick(dc_control *c,uint64_t now,dc_decision *d) {
    if(!c||!d)return -1;
    decision(c,d);
    if(!c->closed&&c->config.idle_ms&&!c->reply_live&&now>=c->last_activity_ms&&now-c->last_activity_ms>=c->config.idle_ms)
        return dc_control_close(c,now,d);
    return 0;
}
