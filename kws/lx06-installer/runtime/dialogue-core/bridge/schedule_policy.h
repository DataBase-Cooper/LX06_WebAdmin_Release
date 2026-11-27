/* Scheduled OPEN admission, local API 1. PROVISIONAL(P-20261111-1343-B).
 * No send, execution, owner acquisition, TTS or result acknowledgement. */
#ifndef LX06_BR_SCHEDULE_POLICY_H
#define LX06_BR_SCHEDULE_POLICY_H
#include "schedule.h"
#include "system_op.h"
/* INTENT's exact identity, CURRENT authenticated caps, scheduled/no wake text.
 * Phone audio close for both modes; silent also needs op.silent=true.
 * system.op additionally needs matching typed rule; silent permits ACK only.
 * Backend MUST still apply system_submit's complete local/path allowlist.
 * scratch wiped; returns existing BR_CAPS result, no new Wire error codes. */
int br_schedule_admit(const br_schedule *,unsigned slot,uint64_t serial,
                       const br_caps *,const void *,size_t,const br_system_rule *,br_json *scratch);
#endif
