#include "command_owner.h"
#include "bridge/result_fingerprint.h"
#include <string.h>

_Static_assert(sizeof(br_command_owner)<4096,"bounded original receipt adapter");
typedef struct {
    const br_session *session;
    const br_inbox *inbox;
    const br_discovery *discovery;
    br_store *store;
    int (*associate)(void *,const br_result *);
    void *inbox_context;
    uint64_t run,epoch,generation;
    unsigned count,rows;
    br_session_ticket ticket;
    unsigned char caps[32];
} command_snapshot;

static int live(const br_command_owner *o,uint64_t run,uint64_t epoch,
                const char *command,const char *task){
    const br_session *s=o->session;
    if(!s||s->busy!=1||!s->live||!s->owned||s->terminal||!run||!epoch||
       s->run!=run||s->epoch!=epoch||!s->count||s->count>BR_SESSION_LIMIT||
       !s->generation||!command||!task||!s->io.receipt||!s->io.reaped||!s->io.close)return 0;
    const br_operation *op=&s->operation;const br_discovery *d=op->discovery;
    const br_session_ticket *ticket=s->tickets+s->count-1;
    return d&&d->epoch==epoch&&d->phase==BR_DISC_READY&&!d->describing&&
        d->caps.ready&&d->caps.connection==epoch&&
        br_caps_has_feature(&d->caps,epoch,"session.command_id")&&
        op->epoch==epoch&&op->plan.oneway&&op->acked&&op->pending&&!op->completed&&
        ticket->generation==s->generation&&
        memchr(ticket->command,0,sizeof ticket->command)&&
        memchr(ticket->task,0,sizeof ticket->task)&&ticket->task[0]&&
        memchr(op->command_id,0,sizeof op->command_id)&&
        memchr(op->task_id,0,sizeof op->task_id)&&
        !strcmp(ticket->command,op->command_id)&&!strcmp(ticket->task,op->task_id)&&
        !strcmp(command,ticket->command)&&!strcmp(task,ticket->task);
}
static int unchanged(const br_command_owner *o,const command_snapshot *p,
                     const char *command,const char *task){
    const br_session *s=o->session;br_inbox *q=o->inbox;unsigned char caps[32];
    if(s!=p->session||q!=p->inbox||!live(o,p->run,p->epoch,command,task)||s->operation.discovery!=p->discovery||
       s->count!=p->count||s->generation!=p->generation||
       memcmp(s->tickets+s->count-1,&p->ticket,sizeof p->ticket)||
       !q||q->store!=p->store||q->blocked||!q->store||q->store->poisoned||
       q->store->directory<0||q->store->lock<0||q->store->schedule||
       q->associate!=p->associate||q->context!=p->inbox_context||q->count!=p->rows)return 0;
    br_result_fingerprint(&p->discovery->caps.document,caps);
    return !memcmp(caps,p->caps,sizeof caps);
}
static int find(const br_command_owner *o,const command_snapshot *p,const br_result **out){
    *out=NULL;
    const br_inbox *q=o->inbox;
    if(q->count>BR_INBOX_ROWS)return BR_INVALID;
    for(unsigned i=0;i<q->count;i++){
        const br_result *r=&q->rows[i].result;
        if(r->origin)continue; /* local receipts/notices never become Ledger */
        if(!br_result_valid(r))return BR_INVALID;
        if(strcmp(r->command_id,p->ticket.command)||strcmp(r->task_id,p->ticket.task))continue;
        if(*out||r->epoch!=p->epoch)return BR_INVALID;
        *out=r;
    }
    return *out?1:0;
}
static void row_hash(const br_result *r,unsigned char out[32]){
    br_result_hash h;br_result_hash_init(&h);br_result_hash_add(&h,r,sizeof(*r));br_result_hash_finish(&h,out);
}
int br_command_owner_receipt(void *context,uint64_t run,uint64_t epoch,
                             const char *command,const char *task){
    br_command_owner *o=context;
    if(!o||o->busy)return BR_INVALID;
    memset(&o->scratch,0,sizeof o->scratch);
    br_inbox *q=o->inbox;
    if(!live(o,run,epoch,command,task)||!q||!q->store||!q->associate)return BR_INVALID;
    if(q->blocked||q->store->poisoned||q->store->directory<0||q->store->lock<0||q->store->schedule)return BR_IO;
    if(q->count>BR_INBOX_ROWS)return BR_INVALID;
    const br_session *s=o->session;
    command_snapshot p={.session=s,.inbox=q,.discovery=s->operation.discovery,.store=q->store,
        .associate=q->associate,.inbox_context=q->context,.run=run,.epoch=epoch,
        .generation=s->generation,.count=s->count,.rows=q->count,.ticket=s->tickets[s->count-1]};
    br_result_fingerprint(&p.discovery->caps.document,p.caps);
    o->busy=1;
    const br_result *row=NULL;int rc=find(o,&p,&row);
    if(rc==1){
        unsigned char before[32],after[32];row_hash(row,before);
        o->scratch=*row;memset(o->scratch.code,0,sizeof o->scratch.code);memset(o->scratch.text,0,sizeof o->scratch.text);
        const br_result *received=NULL;
        rc=br_inbox_ledger_result(q,&o->scratch,&received);
        if(!unchanged(o,&p,command,task))rc=BR_INVALID;
        else if(rc==1){
            const br_result *current=NULL;
            if(find(o,&p,&current)!=1||received!=row||current!=row)rc=BR_INVALID;
            else{row_hash(current,after);if(memcmp(before,after,sizeof before))rc=BR_INVALID;}
        }
    }else if(!unchanged(o,&p,command,task))rc=BR_INVALID;
    memset(&o->scratch,0,sizeof o->scratch);o->busy=0;
    return rc;
}
