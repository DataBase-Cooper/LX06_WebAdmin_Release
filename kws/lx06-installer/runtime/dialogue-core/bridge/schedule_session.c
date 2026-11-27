#include "schedule_session.h"
#include <string.h>

int br_schedule_session_check(const br_schedule *s,unsigned slot,const br_flow *f,
    const br_flow_log_binding *b,const br_flow_session_log_binding *v,const char command[37]){
    if(!br_schedule_storage_valid(s)||s->blocked||s->workspace->busy||slot>=s->count||
       !s->trusted||!s->sampled||!s->reconciled||!s->idle||
       !br_flow_log_binding_valid(b)||!br_flow_session_log_binding_valid(v)||!command||command[36]||
       !f||f->status!=BR_FLOW_NEXT||f->index||f->ticket||f->used||f->checkpoint||
       f->session_epoch||f->session_generation||!f->definition||!f->definition->steps||
       !f->script_id||!f->wake_source||strcmp(f->wake_source,"scheduled")||
       (f->wake_text&&*f->wake_text)||f->now_ms!=s->mono||f->started_ms!=s->mono)return -1;
    const br_schedule_record *r=s->records+slot;
    const br_schedule_task *t=s->tasks+slot;
    if(r->phase!=BR_SCHED_INTENT||!t->enabled||r->auth_blocked||r->remote[0]||
       r->serial!=b->serial||s->owned!=b->serial||s->sequence!=b->serial||
       r->epoch!=v->epoch||memcmp(r->command,command,37)||
       strcmp(s->script,b->script)||strcmp(s->revision,b->revision)||strcmp(t->id,b->task)||
       t->audible!=b->audible||b->silent!=!b->audible||f->silent!=(int)b->silent||
       strcmp(v->script,b->script)||strcmp(v->revision,b->revision)||
       strcmp(v->definition_sha256,b->definition_sha256)||strcmp(v->inputs_sha256,b->inputs_sha256)||
       strcmp(f->script_id,b->script)||v->run!=b->run||f->run!=b->run||
       v->count!=b->count||f->definition->count!=b->count||
       v->source!=BR_LOG_SCHEDULED||v->ctl_run||v->cycle||v->job)return -1;
    /* Original begin validates these definitions; never reinterpret params.
     * Current caps/session/oneway checks still belong to actual dispatch. */
    for(size_t i=0;i<f->definition->count;i++){
        const br_flow_step *step=f->definition->steps+i;
        if(!step->action||step->audio||!strcmp(step->action,"system.op"))return -1;
    }
    return 1;
}
