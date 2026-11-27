/* Local API1, PROVISIONAL(P-20261112-0305-B). Observation only, no recovery
 * actions. Schema9 does NOT persist task/app/hook: original durable owner
 * registry must prove these associations on EVERY query. No guessed context.
 * Serialized, non-realtime. Callbacks cannot mutate, perform IO or reenter
 * other owner/inbox APIs. No production wiring before actual A/C signoff. */
#ifndef LX06_BR_FLOW_SESSION_LEDGER_H
#define LX06_BR_FLOW_SESSION_LEDGER_H
#include "flow_session_log.h"
#include "result_inbox.h"
#define BR_FLOW_SESSION_LEDGER_API 1u
/* Exactly1 proves original run/generation/index/ticket/UUID -> TASK/versions
 * in the original durable registry. Independent of K6's current association.
 * This is a check, never an action to create or repair missing association. */
typedef int (*br_flow_session_ledger_associate)(void *,const br_flow_session_log_binding *,
    unsigned index,const br_flow_session_log_step *,const br_result *identity);
/* Readonly reopened log only. index must have an original PREPARE record.
 * Identity: origin0, exact epoch/UUID, original task/app/hook, empty code/text.
 * 1 borrowed original K6 result until next inbox mutation; 0 absent (unresolved);
 * BR_INVALID/BR_IO on refusal/blocked storage. Failure always clears out.
 * Result presence, historical phases and TTL never prove release or permit
 * continuation, retirement, terminalization, replay or playback. No writes. */
int br_flow_session_ledger_result(br_flow_session_log *,unsigned index,br_inbox *,
    const br_result *identity,br_flow_session_ledger_associate,void *,const br_result **out);
#endif
