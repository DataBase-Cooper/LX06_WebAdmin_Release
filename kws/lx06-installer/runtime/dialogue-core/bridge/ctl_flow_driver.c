#include "ctl_flow_driver.h"
#include <string.h>
_Static_assert(sizeof(br_ctl_flow_driver)<=48,"bounded serial driver");
static int idle(const br_ctl_flow *a){
    return a&&!a->busy&&a->io&&!a->io->busy&&a->io->session&&!a->io->session->busy&&
        a->io->flow&&a->io->session->operation.discovery&&a->fence&&!a->fence->busy&&a->fence->ctl;
}
int br_ctl_flow_driver_init(br_ctl_flow_driver *d,br_ctl_flow *a,uint64_t now){
    if(!d||!idle(a)||a->active||a->io->bound||a->io->active||a->io->session->owned||
       a->io->flow->status!=BR_FLOW_IDLE||!a->io->session->live||
       a->fence->phase!=BR_FENCE_ACTIVE||a->fence->ctl->phase!=BR_CTL_ACTIVE||
       !a->run||!a->epoch||a->run!=a->io->session->run||a->run!=a->fence->run||
       a->run!=a->fence->ctl->run||a->epoch!=a->io->session->epoch||
       a->epoch!=a->fence->epoch||a->epoch!=a->fence->ctl->epoch||
       a->io->session->operation.discovery->epoch!=a->epoch||
       a->io->session->operation.discovery->phase!=BR_DISC_READY||
       a->fence->ctl->caps!=&a->io->session->operation.discovery->caps||
       !a->fence->ctl->caps->ready||a->fence->ctl->caps->connection!=a->epoch||!a->fence->generation||
       a->io->session->generation>a->fence->generation||
       now<a->now_ms||now<a->fence->now_ms||now<a->fence->ctl->now_ms||
       now<a->io->session->operation.discovery->now_ms||now<a->io->flow->now_ms)return -1;
    memset(d,0,sizeof(*d));d->adapter=a;d->run=a->run;d->epoch=a->epoch;d->now_ms=now;return 1;
}
static void close_original(br_ctl_flow_driver *d,uint64_t now){
    br_ctl_flow *a=d->adapter;
    if(a->fence->phase!=BR_FENCE_OFF&&a->fence->phase!=BR_FENCE_CLOSED)
        (void)br_ctl_fence_close(a->fence,d->run,a->fence->cycle,now);
    if(a->io->session->live)(void)br_session_close(a->io->session,d->run,d->epoch);
}
static int draining(const br_ctl *c){return c->phase==BR_CTL_DRAINING||c->phase==BR_CTL_CLOSED;}
static int pump(br_ctl_flow_driver *d,uint64_t now){
    br_ctl_flow *a=d->adapter;br_session *s=a->io->session;br_ctl *c=a->fence->ctl;
    if(s->live)(void)br_session_poll(s,d->run,d->epoch,now);
    if(a->fence->phase!=BR_FENCE_OFF&&a->fence->phase!=BR_FENCE_CLOSED)
        (void)br_ctl_fence_poll(a->fence,d->run,a->fence->cycle,now);
    if(!s->live||draining(c))close_original(d,now);
    if(a->active)(void)br_ctl_flow_poll(a,d->run,a->cycle,a->job,now);
    int rc=br_ctl_poll(c,d->run,now,&s->operation.discovery->scratch);
    if(draining(c))close_original(d,now);
    return rc;
}
static int current(const br_ctl_flow_driver *d,const br_ctl_flow_event *e){
    if(!d||d->busy||!e||e->kind<BR_DRIVER_TICK||e->kind>BR_DRIVER_CLEAR||!idle(d->adapter))return 0;
    const br_ctl_flow *a=d->adapter;const br_ctl *c=a->fence->ctl;const br_session *s=a->io->session;
    if(!e->run||e->run!=d->run||e->run!=a->run||e->run!=c->run||e->run!=s->run||
       !e->epoch||e->epoch!=d->epoch||e->epoch!=a->epoch||e->epoch!=c->epoch||e->epoch!=s->epoch||
       e->epoch!=a->fence->epoch||e->run!=a->fence->run||
       e->cycle!=c->cycle||e->job!=c->job||!e->generation||e->generation!=a->fence->generation)return 0;
    /* Discovery storage belongs to the original connection through drain.
     * Replaced storage is never polled, parsed or wiped by this driver. */
    if(s->operation.discovery->epoch&&s->operation.discovery->epoch!=e->epoch)return 0;
    if(e->kind==BR_DRIVER_RECEIVE&&(!a->active||!a->io->active||!e->ticket||e->ticket!=a->io->ticket))return 0;
    if(e->kind==BR_DRIVER_DISPATCH&&(!a->active||!e->ticket||e->ticket<=a->io->flow->last_ticket))return 0;
    if(e->kind==BR_DRIVER_BARGE&&(a->active||!e->next_cycle||e->next_cycle<=c->last_cycle))return 0;
    if(e->kind==BR_DRIVER_OFFICIAL&&e->enabled!=0&&e->enabled!=1)return 0;
    return e->now_ms>=d->now_ms&&e->now_ms>=a->now_ms&&e->now_ms>=c->now_ms&&
        e->now_ms>=a->fence->now_ms&&e->now_ms>=a->io->flow->now_ms&&
        e->now_ms>=s->operation.discovery->now_ms;
}
int br_ctl_flow_driver_apply(br_ctl_flow_driver *d,const br_ctl_flow_event *e){
    if(!current(d,e))return -1;
    if(e->kind==BR_DRIVER_OFFICIAL&&!e->enabled)return 0;
    br_ctl_flow *a=d->adapter;br_ctl *c=a->fence->ctl;br_discovery *s=a->io->session->operation.discovery;
    d->busy=1;int rc=-1;
    if(e->kind==BR_DRIVER_OFFICIAL||e->kind==BR_DRIVER_DISCONNECT){
        if(e->kind==BR_DRIVER_OFFICIAL)(void)br_ctl_official(c,e->run,e->now_ms,1);
        close_original(d,e->now_ms);rc=pump(d,e->now_ms);
    }else{
        rc=pump(d,e->now_ms);
        switch(e->kind){
        case BR_DRIVER_TICK:break;
        case BR_DRIVER_STATE:rc=br_ctl_state(c,e->run,e->epoch,e->now_ms,e->data,e->length,&s->scratch);break;
        case BR_DRIVER_BARGE:rc=br_ctl_barge(c,e->run,e->next_cycle,e->now_ms);break;
        case BR_DRIVER_ROUTE:rc=br_ctl_window(c,e->run,e->cycle,e->now_ms,e->route,&s->scratch);break;
        case BR_DRIVER_FENCE:rc=br_ctl_fence_receive(a->fence,e->run,e->cycle,e->now_ms,e->data,e->length,&s->packet);break;
        case BR_DRIVER_DISPATCH:rc=br_ctl_flow_dispatch(a,e->run,e->cycle,e->job,e->ticket,e->now_ms,e->command);break;
        case BR_DRIVER_RECEIVE:rc=br_ctl_flow_receive(a,e->run,e->cycle,e->job,e->ticket,e->now_ms,e->data,e->length);break;
        case BR_DRIVER_FINISH:rc=br_ctl_flow_finish(a,e->run,e->cycle,e->job,e->now_ms,e->data,e->length);break;
        case BR_DRIVER_CLEAR:rc=br_ctl_flow_clear(a);break;
        default:break;
        }
    }
    d->now_ms=e->now_ms;d->busy=0;return rc;
}
