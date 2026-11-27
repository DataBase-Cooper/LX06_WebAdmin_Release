#include "ctl.h"
#include <string.h>
_Static_assert(sizeof(br_ctl)<32*1024,"bounded CTL state");
static int boolean(int n){return n==0||n==1;}
static int name_valid(const char *s){
    if(!s)return 0;
    size_t n=0;while(n<=128&&s[n])n++;
    if(!n||n>128)return 0;
    for(size_t i=0;i<n;){
        unsigned c=(unsigned char)s[i++],more=0,v=c,min=0;
        if(c<32||c==127)return 0;
        if(c>=128){
            if(c>=0xc2&&c<=0xdf){more=1;v=c&31;min=128;}
            else if(c>=0xe0&&c<=0xef){more=2;v=c&15;min=2048;}
            else if(c>=0xf0&&c<=0xf4){more=3;v=c&7;min=65536;}else return 0;
            if(more>n-i)return 0;
            while(more--){c=(unsigned char)s[i++];if((c&0xc0)!=0x80)return 0;v=(v<<6)|(c&63);}
            if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
        }
    }
    return 1;
}
static int same(br_text a,br_text b){return a.length==b.length&&!memcmp(a.data,b.data,a.length);}
static int live(const br_ctl *s){return s->caps&&s->caps->ready&&s->caps->connection==s->epoch;}
static void close_window(br_ctl *s){
    if(s->window_owned&&!s->window_closed){s->window_closed=1;s->io.window_close(s->io.context,s->cycle);}
}
static void cancel_work(br_ctl *s){
    if(s->work_owned&&!s->work_cancelled){s->work_cancelled=1;s->io.work_cancel(s->io.context,s->cycle,s->job);}
}
static void erase(br_ctl *s){
    memset(s->state,0,sizeof(s->state));memset(s->s0,0,sizeof(s->s0));memset(s->request,0,sizeof(s->request));
    s->state_length=s->s0_length=s->request_length=0;memset(&s->route,0,sizeof(s->route));
}
static void stop(br_ctl *s,const char *error,int connected){
    if(s->phase==BR_CTL_DRAINING||s->phase==BR_CTL_CLOSED)return;
    s->error=error;close_window(s);cancel_work(s);
    /* Owner teardown must synchronously gate all local IO, even if active. */
    if(connected&&live(s)&&!s->wake_sent&&!s->close_sent){s->close_sent=1;(void)s->io.control(s->io.context,BR_WIRE_CLOSE,s->epoch);}
    if(connected&&!live(s))s->error="bridge_unreachable";
    s->phase=BR_CTL_DRAINING;s->since_ms=s->now_ms;s->io.teardown(s->io.context,s->run);
}
/* A synchronous owner may discover connection loss while our call guard
 * rejects its nested disconnect. Observe revocation before using a callback
 * result for new work; cleanup of the ORIGINAL resources still works offline. */
static int connected(br_ctl *s){
    if(live(s))return 1;
    stop(s,"bridge_unreachable",0);return 0;
}
static int tick(br_ctl *s,uint64_t run,uint64_t now){
    if(!s||run!=s->run||!run||s->phase==BR_CTL_IDLE)return -1;
    if(s->phase==BR_CTL_CLOSED)return 0;
    if(now<s->now_ms){stop(s,"bridge_unreachable",0);return 0;}
    s->now_ms=now;
    if(s->phase!=BR_CTL_DRAINING&&!live(s))stop(s,"bridge_unreachable",0);
    return 1;
}
static int object(br_json *r,const void *data,size_t n){
    if(!data||!n||n>8192||br_json_parse(r,data,n)!=1||br_json_kind(r,1)!=BR_J_OBJECT)return 0;
    for(unsigned k=1;k<=r->count;k++)if(br_json_kind(r,k)==BR_J_KEY&&
        (br_json_key_ci(r,k,"token")||br_json_key_ci(r,k,"authorization")||br_json_key_ci(r,k,"phone_token")))return 0;
    return 1;
}
static int scratch_ok(const br_ctl *s,const br_json *r){return r&&(!s->caps||r!=&s->caps->document);}
static void resume(br_ctl *s,int original){
    s->restore_s0=original;s->phase=BR_CTL_RESUMING;s->since_ms=s->now_ms;
    int sent=s->io.control(s->io.context,BR_WIRE_RESUME,s->epoch);
    if(connected(s)&&sent!=1)stop(s,"bridge_unreachable",1);
}
static void ending(br_ctl *s){
    if(!s->end_op.length||s->ending){stop(s,s->error,1);return;}
    s->ending=1;s->oneway=s->end_oneway;s->route=(br_ctl_route){.kind=BR_CTL_END};
    s->timeout_ms=s->end_timeout;s->since_ms=s->now_ms;s->phase=BR_CTL_START;s->job++;
}
static void fallback(br_ctl *s){if(s->config.timeout_end)ending(s);else resume(s,1);}
int br_ctl_init(br_ctl *s,br_ctl_io io){
    if(!s||!io.quiesce||!io.quiet||!io.window_open||!io.window_close||!io.window_reaped||
       !io.work_start||!io.work_cancel||!io.work_reaped||!io.control||!io.restore||
       !io.teardown||!io.reaped||!io.notice)return -1;
    memset(s,0,sizeof(*s));s->io=io;return 1;
}
static int ctl_begin(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now,const br_caps *caps,
                 const char *app,const char *script,br_ctl_config config){
    if(!s||!s->io.control||(s->phase!=BR_CTL_IDLE&&s->phase!=BR_CTL_CLOSED)||!run||run<=s->last_run||
       !epoch||!caps||!caps->ready||caps->connection!=epoch||!name_valid(app)||!name_valid(script)||
       !boolean(config.declared)||!boolean(config.timeout_end)||config.start_ms<1000||config.start_ms>15000||
       config.max_ms<3000||config.max_ms>15000||(s->last_run&&now<s->now_ms))return -1;
    const br_json *r=&caps->document;unsigned apps=br_json_field(r,1,"apps"),found=0;
    for(unsigned a=br_json_child(r,apps);a&&a<br_json_next(r,apps);a=br_json_next(r,a))
        if(br_json_is(r,br_json_field(r,a,"app"),app)){found=a;break;}
    if(!found)return -1;
    unsigned session=br_json_field(r,found,"session"),cancel=br_json_field(r,session,"cancel");
    if(config.declared&&!br_json_is(r,cancel,"pause_hold")&&!br_json_is(r,cancel,"freeze_only"))return -1;
    br_text end=br_json_text(r,br_json_field(r,session,"end_op"));uint64_t timeout=0;int end_oneway=0;
    if(end.length){
        unsigned ops=br_json_field(r,found,"ops");
        for(unsigned o=br_json_child(r,ops);o&&o<br_json_next(r,ops);o=br_json_next(r,o))
            if(same(end,br_json_text(r,br_json_field(r,o,"name")))){
                (void)br_json_u64(r,br_json_field(r,o,"timeout_ms"),&timeout);
                end_oneway=br_json_kind(r,br_json_field(r,o,"oneway"))==BR_J_TRUE;
            }
        if(!timeout)return -1;
    }
    br_ctl_io io=s->io;uint64_t last_cycle=s->last_cycle;
    memset(s,0,sizeof(*s));s->busy=1;s->io=io;s->last_cycle=last_cycle;s->run=s->last_run=run;
    s->caps=caps;s->epoch=epoch;s->now_ms=now;s->config=config;s->phase=BR_CTL_ACTIVE;
    memcpy(s->script,script,strlen(script)+1);s->app=br_json_text(r,br_json_field(r,found,"app"));
    s->end_op=end;s->end_timeout=timeout;s->end_oneway=end_oneway;return 1;
}
static int ctl_state(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now,const void *data,size_t n,br_json *r){
    if(!s||run!=s->run||epoch!=s->epoch||!scratch_ok(s,r))return -1;
    int rc=-1;
    if(tick(s,run,now)==1&&s->phase==BR_CTL_ACTIVE&&object(r,data,n)){
        memset(s->state,0,sizeof(s->state));memcpy(s->state,data,n);s->state_length=n;rc=1;
    }
    memset(r,0,sizeof(*r));return rc;
}
static int ctl_barge(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t now){
    if(!s||run!=s->run||!cycle||cycle<=s->last_cycle)return -1;
    if(tick(s,run,now)!=1||s->phase!=BR_CTL_ACTIVE)return -1;
    if(!s->config.declared)return 0;
    s->cycle=s->last_cycle=cycle;s->job=0;s->ending=s->ends=s->oneway=s->restore_s0=0;
    s->error=NULL;s->decision=0;memset(s->s0,0,sizeof(s->s0));memcpy(s->s0,s->state,s->state_length);
    s->s0_length=s->state_length;s->phase=BR_CTL_QUIET;s->since_ms=now;
    int q=s->io.quiesce(s->io.context,cycle);
    if(!connected(s))return s->phase;
    int sent=s->io.control(s->io.context,BR_WIRE_CANCEL,s->epoch);
    if(!connected(s))return s->phase;
    if(q!=1||sent!=1)stop(s,"bridge_unreachable",1);
    return s->phase;
}
static int ctl_window(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t now,const br_ctl_route *route,br_json *r){
    if(!s||run!=s->run||cycle!=s->cycle||!scratch_ok(s,r))return -1;
    int rc=-1;
    if(tick(s,run,now)!=1||s->phase!=BR_CTL_WINDOW||!route)goto done;
    if(now-s->since_ms>=(uint64_t)s->config.start_ms+s->config.max_ms){
        s->decision=BR_CTL_TIMEOUT;close_window(s);
        if(connected(s)){s->phase=BR_CTL_RELEASE;s->since_ms=now;}
        rc=s->phase;goto done;
    }
    if(route->kind<BR_CTL_TIMEOUT||route->kind>BR_CTL_FLOW)goto done;
    s->route=(br_ctl_route){.kind=route->kind};s->decision=route->kind;
    if(route->kind==BR_CTL_OP){
        br_caps_plan plan;
        int valid=br_caps_open_operation(s->caps,s->epoch,route->open,route->length,r,&plan)==BR_CAPS_OK;
        if(valid)valid=same(plan.app,s->app);
        if(valid)valid=object(r,route->open,route->length)&&br_json_is(r,br_json_field(r,1,"script_id"),s->script);
        if(!valid){s->decision=BR_CTL_MISS;s->error="unsupported";memset(&s->route,0,sizeof(s->route));}
        else{
            memcpy(s->request,route->open,route->length);s->request_length=route->length;
            s->route.open=s->request;s->route.length=route->length;
            s->timeout_ms=plan.timeout_ms;s->ends=plan.session_end;s->oneway=plan.oneway;
            /* Completing the declared end op must not invoke it a second time. */
            /* A session=end operation already performs the terminating work.
             * Do not send a second end_op (P-20261111-2048-B). Failure also
             * drains/closes: a partial ending operation cannot be resumed. */
            s->ending=plan.session_end||(s->end_op.length&&same(plan.op,s->end_op));
        }
    }else if(route->kind==BR_CTL_FLOW){
        if(!route->flow){s->decision=BR_CTL_MISS;s->error="unsupported";}
        s->route.flow=route->flow;s->route.inputs=route->inputs;s->route.input_count=route->input_count;
        s->timeout_ms=900000;
    }
    close_window(s);
    if(connected(s)){s->phase=BR_CTL_RELEASE;s->since_ms=now;}
    rc=s->phase;
done:
    memset(r,0,sizeof(*r));return rc;
}
static int ctl_work(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now,
                enum br_ctl_proof proof,int ended,const void *data,size_t n,br_json *r){
    if(!s||run!=s->run||cycle!=s->cycle||job!=s->job||!scratch_ok(s,r))return -1;
    int rc=-1;
    if(tick(s,run,now)!=1||s->phase!=BR_CTL_WORK||!boolean(ended))goto done;
    if(now-s->since_ms>=s->timeout_ms){
        s->error="op_timeout";s->work_ok=0;s->phase=BR_CTL_WORK_DRAIN;cancel_work(s);
        (void)connected(s);rc=s->phase;goto done;
    }
    if(proof!=BR_CTL_FAILED&&proof!=(s->route.kind==BR_CTL_FLOW?BR_CTL_FLOW_DONE:
                                    s->oneway?BR_CTL_LEDGER:BR_CTL_READY))goto done;
    if(n&&!object(r,data,n))goto done;
    s->work_ok=proof!=BR_CTL_FAILED;
    if(!s->work_ok)s->error="bridge_unreachable";
    else{
        if(s->route.kind==BR_CTL_FLOW){
            /* The original FLOW already executed and reaped its session=end
             * step. Retire this CTL after drain; never send end_op twice. */
            s->ends=ended;if(ended)s->ending=1;
        }
        memset(s->state,0,sizeof(s->state));s->state_length=n;if(n)memcpy(s->state,data,n);
    }
    s->phase=BR_CTL_WORK_DRAIN;rc=s->phase;
done:
    memset(r,0,sizeof(*r));return rc;
}
static int ctl_resumed(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t epoch,uint64_t now){
    if(!s||run!=s->run||cycle!=s->cycle||epoch!=s->epoch)return -1;
    if(tick(s,run,now)!=1||s->phase!=BR_CTL_RESUMING)return -1;
    if(now-s->since_ms>=6000){stop(s,"op_timeout",1);return s->phase;}
    const void *data=s->restore_s0?s->s0:s->state;size_t n=s->restore_s0?s->s0_length:s->state_length;
    int restored=s->io.restore(s->io.context,data,n);
    if(!connected(s))return s->phase;
    if(restored!=1){stop(s,"bridge_unreachable",1);return s->phase;}
    if(s->restore_s0){memset(s->state,0,sizeof(s->state));memcpy(s->state,s->s0,s->s0_length);s->state_length=s->s0_length;}
    memset(s->s0,0,sizeof(s->s0));s->s0_length=0;memset(s->request,0,sizeof(s->request));s->request_length=0;
    memset(&s->route,0,sizeof(s->route));s->phase=BR_CTL_ACTIVE;return s->phase;
}
static int ctl_official(br_ctl *s,uint64_t run,uint64_t now,int enabled){
    if(!s||run!=s->run||!boolean(enabled))return -1;
    if(!enabled)return 0;
    if(tick(s,run,now)!=1)return s->phase;
    if(s->wake_sent||!live(s))return s->phase;
    /* Official wake remains highest priority during an earlier local drain.
     * Gate locally before sending WAKE. cycle may be 0 before first barge. */
    (void)s->io.quiesce(s->io.context,s->cycle);
    if(!connected(s))return s->phase;
    close_window(s);
    if(!connected(s))return s->phase;
    /* Cancelling an original FLOW may itself close its session. WAKE is the
     * original official closing notification, not new work or a RESUME. Keep
     * its single attempt after cancellation even when that cleanup revoked
     * caps; the transport must still target/refuse the original epoch. */
    cancel_work(s);
    s->wake_sent=1;(void)s->io.control(s->io.context,BR_WIRE_WAKE,s->epoch);
    stop(s,live(s)?NULL:"bridge_unreachable",0);return s->phase;
}
static int ctl_disconnect(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now){
    if(!s||run!=s->run||epoch!=s->epoch)return -1;
    (void)tick(s,run,now);stop(s,"bridge_unreachable",0);return s->phase;
}
static int ctl_poll(br_ctl *s,uint64_t run,uint64_t now,br_json *r){
    if(!s||!run||run!=s->run||!scratch_ok(s,r))return -1;
    (void)tick(s,run,now);memset(r,0,sizeof(*r));
    /* A bounded number of transitions; no callback spin or blocking waits. */
    for(unsigned pass=0;pass<8;pass++){
        uint64_t elapsed=s->now_ms-s->since_ms;
        switch(s->phase){
        case BR_CTL_QUIET:{
            if(elapsed>=1000){stop(s,"op_timeout",1);break;}
            int quiet=s->io.quiet(s->io.context,s->cycle);
            if(!connected(s))return s->phase;
            if(quiet!=1)return s->phase;
            s->window_owned=1;s->window_closed=0;s->phase=BR_CTL_WINDOW;s->since_ms=s->now_ms;
            int opened=s->io.window_open(s->io.context,s->cycle,s->script,s->config.start_ms,s->config.max_ms);
            if(!connected(s))return s->phase;
            if(opened!=1)stop(s,"bridge_unreachable",1);
            break;
        }
        case BR_CTL_WINDOW:
            if(elapsed<(uint64_t)s->config.start_ms+s->config.max_ms)return s->phase;
            s->decision=BR_CTL_TIMEOUT;close_window(s);
            if(connected(s)){s->phase=BR_CTL_RELEASE;s->since_ms=s->now_ms;}
            break;
        case BR_CTL_RELEASE:{
            if(elapsed>=1000){stop(s,"op_timeout",1);break;}
            int reaped=s->io.window_reaped(s->io.context,s->cycle);
            if(!connected(s))return s->phase;
            if(reaped!=1)return s->phase;
            s->window_owned=0;
            if(s->decision==BR_CTL_EXIT){ending(s);break;}
            if(s->decision==BR_CTL_OP||s->decision==BR_CTL_FLOW){
                s->job++;s->phase=BR_CTL_START;s->since_ms=s->now_ms;break;
            }
            if(s->decision!=BR_CTL_TIMEOUT)s->io.notice(s->io.context,s->decision);
            if(!connected(s))return s->phase;
            fallback(s);break;
        }
        case BR_CTL_START:{
            if(elapsed>=s->timeout_ms){s->error="op_timeout";if(s->ending)stop(s,s->error,1);else fallback(s);break;}
            int rc=s->io.work_start(s->io.context,s->cycle,s->job,&s->route,s->end_op,s->now_ms,s->timeout_ms-elapsed);
            if(rc==0){(void)connected(s);return s->phase;}
            s->work_owned=1;s->work_cancelled=0;s->phase=BR_CTL_WORK;
            if(rc!=1){
                /* A failed submit may already have closed the original
                 * session. Preserve its uncertain-send handoff for this
                 * return; the next serial tick observes that prior loss.
                 * A NEW loss inside cancel is observed immediately. Neither
                 * path may run fallback/reap/resume in this call. */
                int was_connected=live(s);
                s->work_ok=0;s->error="bridge_unreachable";s->phase=BR_CTL_WORK_DRAIN;cancel_work(s);
                if(was_connected&&!connected(s))return s->phase;
                if(!live(s))return s->phase;
            }else if(!connected(s))return s->phase;
            break;
        }
        case BR_CTL_WORK:
            if(elapsed<s->timeout_ms)return s->phase;
            s->error="op_timeout";s->work_ok=0;s->phase=BR_CTL_WORK_DRAIN;cancel_work(s);
            if(!connected(s))return s->phase;
            break;
        case BR_CTL_WORK_DRAIN:{
            int connected=live(s);
            int reaped=s->io.work_reaped(s->io.context,s->cycle,s->job);
            /* The original adapter may discover a dead connection while
             * checking actual release. Its nested disconnect is refused by
             * our call guard; observe lost caps AFTER the callback returns.
             * Keep work ownership until a later strict release proof. */
            if(connected&&!live(s)){stop(s,"bridge_unreachable",0);return s->phase;}
            if(reaped!=1)return s->phase;
            s->work_owned=0;
            if(s->ending)stop(s,s->error,1);
            else if(!s->work_ok)fallback(s);
            else if(s->ends)ending(s);
            else resume(s,0);
            break;
        }
        case BR_CTL_RESUMING:
            if(elapsed>=6000){stop(s,"op_timeout",1);break;}return s->phase;
        case BR_CTL_DRAINING:
            if(elapsed>=6000&&!s->error)s->error="op_timeout";
            if(s->window_owned){if(s->io.window_reaped(s->io.context,s->cycle)!=1)return s->phase;s->window_owned=0;}
            if(s->work_owned){if(s->io.work_reaped(s->io.context,s->cycle,s->job)!=1)return s->phase;s->work_owned=0;}
            if(s->io.reaped(s->io.context,s->run)!=1)return s->phase;
            erase(s);s->caps=NULL;s->app=s->end_op=(br_text){0};s->phase=BR_CTL_CLOSED;return s->phase;
        default:return s->phase;
        }
    }
    return s->phase;
}
static int ctl_clear(br_ctl *s){
    if(!s||(s->phase!=BR_CTL_IDLE&&s->phase!=BR_CTL_CLOSED))return 0;
    br_ctl_io io=s->io;uint64_t run=s->last_run,cycle=s->last_cycle,now=s->now_ms;
    memset(s,0,sizeof(*s));s->io=io;s->last_run=run;s->last_cycle=cycle;s->now_ms=now;return 1;
}

/* The sole serial CTL owner remains locked through all owner callbacks. In
 * particular quiesce/window_close/work_cancel/control/teardown/reaped cannot
 * recursively accept EXIT, barge or official events or erase borrowed state.
 * This is a call guard, NOT a new speech dedup scope, timer or resource proof.
 * Init still requires fresh storage; it cannot inspect an uninitialized guard. */
static int enter(br_ctl *s){if(!s||s->busy)return 0;s->busy=1;return 1;}
static int leave(br_ctl *s,int rc){s->busy=0;return rc;}
int br_ctl_begin(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now,const br_caps *caps,
    const char *app,const char *script,br_ctl_config config){
    if(!enter(s))return -1;
    return leave(s,ctl_begin(s,run,epoch,now,caps,app,script,config));
}
int br_ctl_state(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now,const void *data,size_t n,br_json *r){
    if(!enter(s))return -1;
    return leave(s,ctl_state(s,run,epoch,now,data,n,r));
}
int br_ctl_barge(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t now){
    if(!enter(s))return -1;
    return leave(s,ctl_barge(s,run,cycle,now));
}
int br_ctl_window(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t now,const br_ctl_route *route,br_json *r){
    if(!enter(s))return -1;
    return leave(s,ctl_window(s,run,cycle,now,route,r));
}
int br_ctl_work(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now,
    enum br_ctl_proof proof,int ended,const void *data,size_t n,br_json *r){
    if(!enter(s))return -1;
    return leave(s,ctl_work(s,run,cycle,job,now,proof,ended,data,n,r));
}
int br_ctl_resumed(br_ctl *s,uint64_t run,uint64_t cycle,uint64_t epoch,uint64_t now){
    if(!enter(s))return -1;
    return leave(s,ctl_resumed(s,run,cycle,epoch,now));
}
int br_ctl_official(br_ctl *s,uint64_t run,uint64_t now,int enabled){
    if(!enter(s))return -1;
    return leave(s,ctl_official(s,run,now,enabled));
}
int br_ctl_disconnect(br_ctl *s,uint64_t run,uint64_t epoch,uint64_t now){
    if(!enter(s))return -1;
    return leave(s,ctl_disconnect(s,run,epoch,now));
}
int br_ctl_poll(br_ctl *s,uint64_t run,uint64_t now,br_json *r){
    if(!enter(s))return -1;
    return leave(s,ctl_poll(s,run,now,r));
}
int br_ctl_clear(br_ctl *s){
    if(!enter(s))return 0;
    return leave(s,ctl_clear(s));
}
