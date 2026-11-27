#include "flow_session.h"
#include <string.h>
_Static_assert(sizeof(br_flow_session)<=160,"bounded borrowed IO adapter");
static int identity(const br_flow_session *a,uint64_t run){
    return a&&!a->busy&&a->bound&&run&&a->run==run&&a->flow->run==run&&
        a->session->run==a->session_run&&a->session->epoch==a->epoch&&
        a->flow->session_epoch==a->epoch&&a->flow->session_generation==a->generation;
}
static int original(const br_flow_session *a){
    const br_session *s=a->session;
    return a->active&&s->count==a->slot+1&&s->generation==a->generation&&
        s->tickets[a->slot].generation==a->generation&&
        !strcmp(s->tickets[a->slot].command,a->flow->command);
}
static void cancel(void *p,uint64_t ticket,uint64_t now){
    br_flow_session *a=p;br_session *s=a->session;(void)now;
    if(a->active&&a->ticket==ticket&&original(a)&&s->live)
        (void)br_session_close_ticket(s,a->session_run,a->epoch,a->slot,a->generation);
}
static int send_frame(void *p,uint64_t session_run,uint64_t epoch,const void *wire,size_t n){
    br_flow_session *a=p;
    /* Called only AFTER the original session committed its ticket. Mark the
     * resource active even on failure: actual reap must still use that ticket. */
    a->active=1;
    if(!a->busy||session_run!=a->session_run||epoch!=a->epoch||!original(a))return -1;
    return a->io.send(a->io.context,a->run,session_run,a->ticket,epoch,a->generation,wire,n);
}
static int start(void *p,uint64_t ticket,uint64_t epoch,int local,const void *data,size_t n,
                  uint64_t now,uint64_t remaining){
    br_flow_session *a=p;br_session *s=a->session;br_flow *f=a->flow;(void)remaining;
    if(!a->busy||!a->bound||a->active||!ticket||f->ticket!=ticket||f->run!=a->run||
       epoch!=a->epoch||local||s->run!=a->session_run||s->epoch!=epoch||
       !s->live||s->owned||s->generation>a->generation)return -1;
    a->ticket=ticket;a->slot=s->count;
    int rc=br_session_send(s,a->session_run,epoch,a->generation,now,data,n,
                            a->wire,BR_FLOW_SESSION_WIRE,send_frame,a);
    memset(a->wire,0,BR_FLOW_SESSION_WIRE);
    return rc==BR_OP_PREPARED?1:-1;
}
static int reaped(void *p,uint64_t ticket,uint64_t now){
    br_flow_session *a=p;br_session *s=a->session;
    if(!a->busy||ticket!=a->ticket)return 0;
    /* Encoder refusal admitted no business ticket and handed off no bytes.
     * This says nothing about the separate session close owner's resources. */
    if(!a->active)return 1;
    if(!original(a))return 0;
    int rc=s->live?br_session_release(s,a->session_run,a->epoch,now):
        br_session_release_closed(s,a->session_run,a->epoch,a->slot,a->generation);
    if(rc!=1&&!(rc==2&&s->live))return 0;
    a->active=0;return 1;
}
int br_flow_session_init(br_flow_session *a,br_flow *f,br_session *s,void *wire,size_t cap,
                          br_flow_session_io io){
    if(!a||!f||!s||!wire||cap!=BR_FLOW_SESSION_WIRE||!io.send||!io.event)return -1;
    memset(a,0,sizeof(*a));a->flow=f;a->session=s;a->wire=wire;a->io=io;
    memset(wire,0,cap);return br_flow_init(f,(br_flow_io){a,start,cancel,reaped});
}
int br_flow_session_bind(br_flow_session *a,uint64_t run,uint64_t session_run,
                          uint64_t epoch,uint64_t generation){
    if(!a||a->busy||a->bound||!run||!session_run||!epoch||!generation)return -1;
    br_session *s=a->session;
    if(s->busy||!s->live||s->owned||s->ended||s->run!=session_run||s->epoch!=epoch||
       s->generation>generation||!s->operation.discovery||
       s->operation.discovery->epoch!=epoch||s->operation.discovery->phase!=BR_DISC_READY)return -1;
    if(br_flow_use_session(a->flow,run,epoch,generation)!=1)return -1;
    a->run=run;a->session_run=session_run;a->epoch=epoch;a->generation=generation;
    a->bound=1;return 1;
}
int br_flow_session_dispatch(br_flow_session *a,uint64_t run,uint64_t ticket,uint64_t now,const char *command){
    if(!identity(a,run)||a->session->busy||!a->session->live)return -1;
    a->busy=1;
    br_discovery *d=a->session->operation.discovery;
    int rc=br_flow_dispatch(a->flow,run,ticket,a->epoch,command,now,&d->caps,&d->scratch);
    a->busy=0;return rc;
}
static void deliver(br_flow_session *a,uint64_t now,int event){
    br_session *s=a->session;
    if(event){
        s->busy=1;a->io.event(a->io.context,a->run,a->ticket,&s->operation,event);s->busy=0;
    }
    (void)br_flow_operation(a->flow,a->run,a->ticket,now,&s->operation,event,
                           &s->operation.discovery->scratch);
}
int br_flow_session_receive(br_flow_session *a,uint64_t run,uint64_t ticket,uint64_t now,
                             const void *data,size_t n){
    if(!identity(a,run)||a->session->busy||!a->active||!ticket||ticket!=a->ticket||
       (a->flow->status!=BR_FLOW_WAIT&&a->flow->status!=BR_FLOW_DRAINING)||
       now<a->flow->now_ms||!original(a))return -1;
    a->busy=1;
    br_session *s=a->session;
    if(s->live&&s->operation.discovery->epoch!=a->epoch)cancel(a,ticket,now);
    int event=s->live?br_session_receive(s,a->session_run,a->epoch,now,data,n):BR_OP_CLOSED;
    deliver(a,now,event);int rc=a->flow->status;a->busy=0;return rc;
}
int br_flow_session_poll(br_flow_session *a,uint64_t run,uint64_t now,int preempt){
    if(!identity(a,run)||a->session->busy||now<a->flow->now_ms||(preempt!=0&&preempt!=1))return -1;
    a->busy=1;br_flow *f=a->flow;br_session *s=a->session;
    if(a->active&&s->live&&s->operation.discovery->epoch!=a->epoch)cancel(a,a->ticket,now);
    if(!preempt&&a->active&&f->status==BR_FLOW_WAIT){
        int event=s->live?br_session_poll(s,a->session_run,a->epoch,now):BR_OP_CLOSED;
        deliver(a,now,event);
    }
    /* A connection lost during success drain cannot authorize another step.
     * A loss detected inside release is handled at the next serial poll. */
    if(!s->live&&f->status==BR_FLOW_DRAINING&&
       (f->after_drain==BR_FLOW_NEXT||f->after_drain==BR_FLOW_DONE))preempt=1;
    int rc=br_flow_poll(f,run,now,preempt,&s->operation.discovery->scratch);
    a->busy=0;return rc;
}
int br_flow_session_event(br_flow_session *a,uint64_t run,uint64_t ticket,uint64_t now,int event){
    if(!identity(a,run)||a->session->busy||!a->active||!ticket||ticket!=a->ticket||
       !original(a)||now<a->flow->now_ms||
       (a->flow->status!=BR_FLOW_WAIT&&a->flow->status!=BR_FLOW_DRAINING))return -1;
    br_operation *op=&a->session->operation;
    /* The shared FLOW consumer additionally verifies phase, original response
     * kind and typed result. ACK never satisfies terminal/reap. */
    if(event!=BR_OP_ACK&&event!=BR_OP_READY&&event!=BR_OP_ERROR&&
       event!=BR_OP_TIMEOUT&&event!=BR_OP_CLOSED&&event!=BR_OP_TASK)return -1;
    if(event==BR_OP_CLOSED||event==BR_OP_TIMEOUT){
        /* Original session close/timeout erases the operation. Current owned
         * ticket is still exact; a live session cannot claim this event. */
        if(a->session->live||op->epoch||!a->session->error)return -1;
        if(event==BR_OP_TIMEOUT&&strcmp(a->session->error,"op_timeout")&&
           strcmp(a->session->error,"bridge_connect_timeout"))return -1;
    }else if(op->last_event!=event||strcmp(op->command_id,a->flow->command))return -1;
    a->busy=1;deliver(a,now,event);int rc=a->flow->status;a->busy=0;return rc;
}
int br_flow_session_clear(br_flow_session *a){
    if(!a||a->busy||a->active)return -1;
    if(br_flow_clear(a->flow)!=1)return -1;
    a->run=a->session_run=a->epoch=a->generation=a->ticket=0;a->slot=0;a->bound=0;
    memset(a->wire,0,BR_FLOW_SESSION_WIRE);return 1;
}
