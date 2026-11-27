#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "../doubao/src/text_match.h"
#include <string.h>

static void light(dc_session *s) {
    if(s->emitted_state==s->control.state)return;
    s->emitted_state=s->control.state;
    s->ops.light(s->ops.context,s->control.tag,++s->light_sequence,s->control.state);
}
static int apply(dc_session *s,const dc_decision *d,uint64_t now) {
    int failed=0;
    /* This order is a contract: no socket write, process signal, SDK call or
     * light operation may run before local audio has been invalidated. */
    if(d->actions&DC_ACT_STOP)s->ops.stop_local(s->ops.context,d->cancelled.session_epoch?d->cancelled:s->control.tag);
    if(d->actions&DC_ACT_CANCEL)s->ops.cancel_remote(s->ops.context,d->cancelled);
    if(d->actions&DC_ACT_END)s->ops.close_remote(s->ops.context);
    if(d->actions&DC_ACT_QUESTION) {
        dc_tag tag=dc_control_reply(&s->control,d->current.utterance_id?d->current.utterance_id:1,now);
        /* The question identity is the recognizer's utterance. Callers set it
         * before apply; no provider is permitted to allocate a generation. */
        if(tag.session_epoch&&s->ops.question(s->ops.context,tag,d->question)) {
            dc_decision failure;
            failed=1;
            (void)dc_control_close(&s->control,now,&failure);(void)apply(s,&failure,now);
        }
    }
    light(s);
    return failed?-1:0;
}
static int normalize(const char *in,char *out,size_t cap) {
    /* The same UTF-8 span parser as controls trims punctuation. For echo
     * matching strip() with no words leaves inner punctuation untouched. */
    return dc_control_strip(out,cap,in,"");
}
static int exact(const char *a,const char *b) {
    if(!*a||!*b)return 0;
    char x[2048],y[2048];
    if(normalize(a,x,sizeof x)||normalize(b,y,sizeof y))return 0;
    if(!strcmp(x,y))return 1;
    return xz_text_is_echo(a,b);
}
static int echo(dc_session *s,const char *text,uint64_t now) {
    if(now>s->echo_until_ms)return 0;
    return exact(text,s->recent)||exact(text,s->greeting)||
        xz_self_echo(text,s->recent,s->control.config.interrupt,s->control.config.goodbye,"");
}
int dc_session_open(dc_session *s,uint64_t epoch,const dc_config *config,const dc_session_ops *ops,const char *wake,uint64_t now) {
    if(!s||!ops||!ops->stop_local||!ops->cancel_remote||!ops->question||!ops->close_remote||!ops->light)return -1;
    memset(s,0,sizeof *s);s->ops=*ops;s->emitted_state=DC_CLOSED;
    if(wake){size_t n=strnlen(wake,sizeof s->wake);if(n==sizeof s->wake)return -1;memcpy(s->wake,wake,n+1);}
    if(dc_control_open(&s->control,epoch,config,now))return -1;
    light(s);return 0;
}
int dc_session_ready(dc_session *s,uint64_t now) {
    if(!s||s->control.closed)return -1;
    if(s->control.reply_live)return 0;
    int r=dc_control_state(&s->control,s->control.tag,DC_LISTENING,now);light(s);return r;
}
dc_tag dc_session_reply(dc_session *s,uint64_t utterance,uint64_t now) {
    dc_tag invalid={0};if(!s)return invalid;
    dc_tag tag=dc_control_reply(&s->control,utterance,now);light(s);return tag;
}
dc_tag dc_session_greeting(dc_session *s,const char *text,uint64_t now) {
    dc_tag invalid={0};if(!s||!text||strnlen(text,sizeof s->greeting)==sizeof s->greeting)return invalid;
    memcpy(s->greeting,text,strlen(text)+1);s->greeting_tag=dc_session_reply(s,1,now);return s->greeting_tag;
}
int dc_session_text_interrupt(dc_session *s,dc_tag source,const char *text,int final,uint64_t now,int *matched) {
    if(matched)*matched=0;
    if(!s||!text||strnlen(text,DC_QUESTION_CAP)==DC_QUESTION_CAP)return -1;
    /* Once a serial has finalized, neither a late partial nor a replayed
     * final may trigger controls, even if tagged with the current reply. */
    if(source.utterance_id&&source.utterance_id<=s->control.last_final_utterance)return 0;
    int is_echo=echo(s,text,now);
    if(final&&!s->control.last_final_utterance&&s->wake[0]&&exact(text,s->wake))return 0;
    /* A genuine complete first question may overlap the greeting. Preserve
     * all of it; never replace it with the first provisional clause. */
    if(final&&!is_echo&&source.utterance_id&&source.session_epoch==s->control.tag.session_epoch&&
       source.reply_generation==s->control.tag.reply_generation&&
       dc_tag_equal(s->greeting_tag,s->control.tag)&&s->control.reply_live&&text[0]) {
        char normalized[DC_QUESTION_CAP];
        if(normalize(text,normalized,sizeof normalized))return -1;
        if(!normalized[0])return 0;
        /* Preflight the actual final operation before stopping anything. */
        char question[DC_QUESTION_CAP];
        if(dc_control_strip(question,sizeof question,text,s->control.config.interrupt))return -1;
        /* Goodbye closes through control in one stop/cancel/end decision.
         * Do not pre-cancel it as a question and then stop a second time. */
        if(!xz_text_matches_list(text,s->control.config.goodbye)) {
            dc_decision stop={.actions=DC_ACT_STOP|DC_ACT_CANCEL,.cancelled=s->control.tag};
            s->control.reply_live=0;s->control.last_cancelled=s->control.tag.reply_generation;
            s->control.interrupted_utterance=source.utterance_id;s->control.state=DC_LISTENING;
            apply(s,&stop,now);
        }
    }
    dc_decision d;int r=dc_control_text(&s->control,source,text,final,is_echo,now,&d);
    if(r>=0&&matched)*matched=(int)d.interrupt_matched;
    if(r>0){d.current.utterance_id=source.utterance_id;if(apply(s,&d,now))return -1;}return r;
}
int dc_session_text(dc_session *s,dc_tag source,const char *text,int final,uint64_t now) {
    return dc_session_text_interrupt(s,source,text,final,now,NULL);
}
int dc_session_voice(dc_session *s,dc_tag source,int near_end,int is_echo,uint64_t now) {
    if(!s)return -1;
    if(source.utterance_id&&source.utterance_id<=s->control.last_final_utterance)return 0;
    dc_decision d;int r=dc_control_voice(&s->control,source,near_end,is_echo,now,&d);
    if(r>0)apply(s,&d,now);
    return r;
}
int dc_session_speaking(dc_session *s,dc_tag tag,uint64_t now) {
    if(!s||!s->control.reply_live)return -1;
    int r=dc_control_state(&s->control,tag,DC_SPEAKING,now);light(s);return r;
}
int dc_session_waiting(dc_session *s,dc_tag tag,uint64_t now) {
    if(!s||!s->control.reply_live)return -1;
    int r=dc_control_state(&s->control,tag,DC_THINKING,now);light(s);return r;
}
int dc_session_reply_done(dc_session *s,dc_tag tag,uint64_t now) {
    if(!s)return -1;
    int r=dc_control_finish_reply(&s->control,tag,now);
    if(!r)s->echo_until_ms=now+5000;
    light(s);return r;
}
void dc_session_spoken(dc_session *s,dc_tag tag,const char *text,uint64_t now) {
    if(!s||!text||!dc_tag_equal(tag,s->control.tag))return;
    size_t n=strnlen(text,1441);if(n>1440)return;
    size_t used=strlen(s->recent);
    if(used+n>1440){size_t drop=used+n-1440;while(drop<used&&((unsigned char)s->recent[drop]&0xc0)==0x80)drop++;memmove(s->recent,s->recent+drop,used-drop+1);}
    strcat(s->recent,text);s->echo_until_ms=now+50000;
}
int dc_session_close(dc_session *s,uint64_t now) {
    if(!s)return -1;
    dc_decision d;int r=dc_control_close(&s->control,now,&d);if(r>0)apply(s,&d,now);return r;
}
int dc_session_finish_close(dc_session *s,uint64_t now) {
    if(!s)return -1;
    dc_decision d;int r=dc_control_finish_close(&s->control,now,&d);if(r>0)light(s);return r;
}
int dc_session_tick(dc_session *s,uint64_t now) {
    if(!s)return -1;
    dc_decision d;int r=dc_control_tick(&s->control,now,&d);if(r>0)apply(s,&d,now);return r;
}
