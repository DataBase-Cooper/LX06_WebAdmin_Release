#include "ctl_flow.h"
#include <string.h>
_Static_assert(sizeof(br_ctl_flow)<=104,"bounded CTL FLOW binding");
static int identity(const br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job){
    if(!a||a->busy||!a->io||!a->fence||!a->fence->ctl)return 0;
    const br_ctl *c=a->fence->ctl;const br_session *s=a->io->session;
    return run&&cycle&&job&&run==a->run&&run==c->run&&run==s->run&&
        cycle==c->cycle&&cycle==a->fence->cycle&&job==c->job&&
        a->epoch==s->epoch&&a->epoch==c->epoch&&a->epoch==a->fence->epoch&&
        !a->io->busy&&!s->busy&&!a->fence->busy;
}
static int original(const br_ctl_flow *a){
    return a->active&&a->cycle==a->fence->ctl->cycle&&a->job==a->fence->ctl->job&&
        a->io->bound&&a->io->run==a->flow_run&&a->io->session_run==a->run&&
        a->io->epoch==a->epoch&&a->io->generation==a->generation&&
        a->io->flow->run==a->flow_run;
}
static int clock_ok(const br_ctl_flow *a,uint64_t now){
    return now>=a->now_ms&&now>=a->fence->ctl->now_ms&&
        now>=a->fence->now_ms&&now>=a->io->flow->now_ms;
}
static int frozen(const br_ctl_flow *a){
    const br_session *s=a->io->session;const br_ctl_fence *f=a->fence;
    return s->live&&!s->ended&&s->operation.discovery&&
        s->operation.discovery->epoch==a->epoch&&f->phase==BR_FENCE_FROZEN&&
        f->acked&&(!a->active||f->generation==a->generation)&&
        f->ctl->caps==&s->operation.discovery->caps;
}
static int terminal(const br_ctl_flow *a){
    return a->io->flow->status>=BR_FLOW_DONE&&!a->io->active&&!a->io->session->owned;
}
int br_ctl_flow_init(br_ctl_flow *a,br_flow_session *io,br_ctl_fence *f,uint64_t now){
    if(!a||!io||io->busy||io->bound||io->active||!io->flow||!io->session||!f||f->busy||!f->ctl)return -1;
    br_session *s=io->session;br_ctl *c=f->ctl;
    if(s->busy||!s->live||s->owned||s->ended||io->flow->status!=BR_FLOW_IDLE||
       f->phase!=BR_FENCE_ACTIVE||c->phase!=BR_CTL_ACTIVE||!c->run||
       c->run!=s->run||c->run!=f->run||c->epoch!=s->epoch||c->epoch!=f->epoch||
       !s->operation.discovery||c->caps!=&s->operation.discovery->caps||
       s->operation.discovery->epoch!=s->epoch||s->generation>f->generation||
       (s->script[0]&&strcmp(s->script,c->script))||now<c->now_ms||now<f->now_ms)return -1;
    if(br_session_poll(s,s->run,s->epoch,now)!=BR_OP_NONE||
       br_ctl_fence_poll(f,f->run,f->cycle,now)!=1)return -1;
    memset(a,0,sizeof(*a));a->io=io;a->fence=f;a->run=s->run;a->epoch=s->epoch;a->now_ms=now;
    return 1;
}
int br_ctl_flow_start(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t flow_run,uint64_t now,const char *source,const char *text){
    if(!identity(a,run,cycle,job)||a->active||!flow_run||!clock_ok(a,now))return -1;
    br_ctl *c=a->fence->ctl;br_flow_session *io=a->io;
    if(c->phase!=BR_CTL_START||c->route.kind!=BR_CTL_FLOW||!c->route.flow||
       c->window_owned||c->work_owned||c->work_cancelled||io->bound||io->session->owned||
       c->since_ms>now||now-c->since_ms>=c->timeout_ms||
       a->fence->phase!=BR_FENCE_FROZEN||!io->session->live||
       flow_run<=io->flow->last_run)return -1;
    if(!a->fence->acked)return 0;
    if(!frozen(a))return -1;
    a->busy=1;
    int rc=br_flow_begin(io->flow,flow_run,now,c->route.flow,c->route.inputs,c->route.input_count,
        c->script,source,text,&io->session->operation.discovery->scratch);
    if(rc==1){
        rc=br_flow_session_bind(io,flow_run,run,a->epoch,a->fence->generation);
        /* A mode refusal (local/audio) admitted no ticket or bytes. End the
         * unbound candidate through original FLOW, so it can be cleared. */
        if(rc!=1)(void)br_flow_poll(io->flow,flow_run,now,1,&io->session->operation.discovery->scratch);
    }
    a->busy=0;
    if(rc!=1)return -1;
    a->cycle=cycle;a->job=job;a->flow_run=flow_run;a->generation=a->fence->generation;
    a->now_ms=now;a->active=1;a->cancelled=a->delivered=a->released=0;return 1;
}
static int work(const br_ctl_flow *a){
    const br_ctl *c=a->fence->ctl;
    return original(a)&&!a->cancelled&&!a->delivered&&!c->work_cancelled&&c->work_owned&&
        c->phase==BR_CTL_WORK;
}
int br_ctl_flow_dispatch(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t ticket,uint64_t now,const char *command){
    if(!identity(a,run,cycle,job)||!work(a)||!clock_ok(a,now)||!frozen(a))return -1;
    a->busy=1;int rc=br_flow_session_dispatch(a->io,a->flow_run,ticket,now,command);a->busy=0;
    if(rc>=0)a->now_ms=now;
    return rc;
}
int br_ctl_flow_receive(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t ticket,uint64_t now,const void *data,size_t n){
    if(!identity(a,run,cycle,job)||!work(a)||!clock_ok(a,now)||!frozen(a))return -1;
    a->busy=1;int rc=br_flow_session_receive(a->io,a->flow_run,ticket,now,data,n);a->busy=0;
    if(rc>=0)a->now_ms=now;
    return rc;
}
int br_ctl_flow_poll(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now){
    if(!identity(a,run,cycle,job)||!original(a)||!clock_ok(a,now))return -1;
    br_ctl *c=a->fence->ctl;
    if(c->work_cancelled||c->phase==BR_CTL_DRAINING||c->phase==BR_CTL_CLOSED||
       (c->phase==BR_CTL_WORK&&now-c->since_ms>=c->timeout_ms)||
       (!terminal(a)&&!frozen(a)))a->cancelled=1;
    a->busy=1;int rc=br_flow_session_poll(a->io,a->flow_run,now,a->cancelled);a->busy=0;
    if(rc>=0)a->now_ms=now;
    return rc;
}
int br_ctl_flow_cancel(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now){
    if(!identity(a,run,cycle,job)||!original(a)||!clock_ok(a,now))return -1;
    a->cancelled=1;return br_ctl_flow_poll(a,run,cycle,job,now);
}
int br_ctl_flow_finish(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t now,const void *data,size_t n){
    if(!identity(a,run,cycle,job)||!original(a)||a->delivered||
       a->fence->ctl->phase!=BR_CTL_WORK||!a->fence->ctl->work_owned||
       a->fence->ctl->work_cancelled||!terminal(a)||!clock_ok(a,now))return -1;
    br_session *s=a->io->session;br_ctl *c=a->fence->ctl;
    a->busy=1;
    int rc;
    if(a->cancelled||!s->live||!c->caps||!c->caps->ready||
       c->caps->connection!=a->epoch||now-c->since_ms>=c->timeout_ms){
        (void)br_ctl_fence_close(a->fence,run,cycle,now);rc=c->phase;
    }else if(a->fence->phase!=BR_FENCE_FROZEN||!a->fence->acked||
             a->fence->generation!=a->generation)rc=-1;
    else rc=br_ctl_work(c,run,cycle,job,now,a->io->flow->status==BR_FLOW_DONE?
        BR_CTL_FLOW_DONE:BR_CTL_FAILED,s->ended,data,n,&s->operation.discovery->scratch);
    a->busy=0;
    if(rc==BR_CTL_WORK_DRAIN||rc==BR_CTL_DRAINING){a->delivered=1;a->now_ms=now;}
    return rc;
}
int br_ctl_flow_reaped(br_ctl_flow *a,uint64_t run,uint64_t cycle,uint64_t job){
    if(!identity(a,run,cycle,job))return -1;
    const br_ctl *c=a->fence->ctl;
    if(c->phase!=BR_CTL_WORK_DRAIN&&c->phase!=BR_CTL_DRAINING)return -1;
    /* A rejected START admitted no FLOW or business bytes. */
    if(!a->active)return 1;
    if(!original(a)||(!a->delivered&&!a->cancelled)||!terminal(a))return 0;
    a->released=1;return 1;
}
int br_ctl_flow_clear(br_ctl_flow *a){
    if(!a||a->busy||!a->io||a->io->busy||a->fence->busy||a->fence->ctl->work_owned||
       (a->active&&(!a->released||!terminal(a))))return -1;
    if(br_flow_session_clear(a->io)!=1)return -1;
    a->active=a->cancelled=a->delivered=a->released=0;return 1;
}
