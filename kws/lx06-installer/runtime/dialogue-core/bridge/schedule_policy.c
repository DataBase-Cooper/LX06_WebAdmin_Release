#include "schedule_policy.h"
#include <string.h>
int br_schedule_admit(const br_schedule *s,unsigned slot,uint64_t serial,const br_caps *caps,
                       const void *data,size_t length,const br_system_rule *rule,br_json *scratch){
    if(!scratch||!s||!caps||scratch==&caps->document)return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(s->blocked||slot>=s->count||!serial)goto end;
    const br_schedule_record *r=s->records+slot;
    if(r->phase!=BR_SCHED_INTENT||r->serial!=serial||s->owned!=serial||
       br_json_parse(scratch,data,length)!=1)goto end;
    uint64_t epoch=0;
    if(!br_json_is(scratch,br_json_field(scratch,1,"script_id"),s->script)||
       !br_json_is(scratch,br_json_field(scratch,1,"wake_source"),"scheduled")||
       !br_json_is(scratch,br_json_field(scratch,1,"command_id"),r->command)||
       !br_json_u64(scratch,br_json_field(scratch,1,"epoch"),&epoch)||epoch!=r->epoch)goto end;
    int local=br_json_is(scratch,br_json_field(scratch,1,"action"),"system.op");
    if(local){
        unsigned params=br_json_field(scratch,1,"params");
        if(!rule||!rule->op||!rule->timeout_ms||rule->timeout_ms>120000||
           rule->kind<BR_SYSTEM_TTS||rule->kind>BR_SYSTEM_ACK||
           !br_json_is(scratch,br_json_field(scratch,params,"op"),rule->op)||
           (!s->tasks[slot].audible&&rule->kind!=BR_SYSTEM_ACK))goto end;
        rc=br_caps_system_request(caps,epoch,data,length,scratch);
    }else{
        br_caps_plan plan;
        rc=br_caps_open_operation(caps,epoch,data,length,scratch,&plan);
        if(rc==BR_CAPS_OK&&(plan.requested_audio||(!s->tasks[slot].audible&&!plan.silent)))rc=BR_CAPS_INVALID;
    }
end:memset(scratch,0,sizeof(*scratch));return rc;
}
