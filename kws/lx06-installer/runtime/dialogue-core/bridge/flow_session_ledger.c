#include "flow_session_ledger.h"
#include <string.h>
int br_flow_session_ledger_result(br_flow_session_log *j,unsigned index,br_inbox *q,
    const br_result *identity,br_flow_session_ledger_associate associate,void *context,const br_result **out){
    if(!out)return BR_INVALID;
    *out=NULL;
    if(!j||j->busy||index>=BR_FLOW_STEPS||!q||!associate||!br_result_valid(identity)||identity->code[0]||identity->text[0])
        return BR_INVALID;
    if(j->blocked||!j->store||j->store->poisoned||q->blocked||!q->store||q->store->poisoned)return BR_IO;
    if(!j->readonly||j->phase==BR_FLOW_SESSION_LOG_BOUND||index>=j->binding.count||index>j->index||
       identity->epoch!=j->binding.epoch||strcmp(identity->command_id,j->steps[index].command))return BR_INVALID;
    const br_flow_session_log *snapshot=NULL;
    if(br_flow_session_log_recovery(j,&snapshot)!=1)return j->blocked?BR_IO:BR_INVALID;
    j->busy=1;
    int rc=BR_INVALID;
    if(associate(context,&snapshot->binding,index,snapshot->steps+index,identity)==1)
        rc=br_inbox_ledger_result(q,identity,out);
    j->busy=0;return rc;
}
