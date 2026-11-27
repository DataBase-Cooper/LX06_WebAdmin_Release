/* T18 isolated generic stepper, API 7 (revision12 row-list parameters).
 * PROVISIONAL(P-20261111-0532-B/P-20261111-1301-B/P-20261111-1734-B).
 * A supplies immutable parsed definitions/validated inputs. No settings,
 * matcher, phone Ledger, sockets, playback or production profile attachment.
 * Serial calls, disjoint buffers, allocate state/scratch off realtime stack.
 * P-20261111-2300-B: fixed-session owner may reuse discovery.scratch for
 * FLOW calls. start may borrow it synchronously via session_prepare, reaped
 * via session_release: FLOW retains no scratch view across these callbacks.
 * Finish session_receive before flow_operation; operation.response MUST stay
 * disjoint. No FLOW reentry, async scratch retention or caps mutation. */
#ifndef LX06_BR_FLOW_H
#define LX06_BR_FLOW_H
#include "operation.h"
#include "system_op.h"
#define BR_FLOW_STEPS 16u
#define BR_FLOW_ARENA 32768u
enum br_flow_source { BR_FLOW_LITERAL=0,BR_FLOW_INPUT,BR_FLOW_JSON,BR_FLOW_TEXT };
enum br_flow_status { BR_FLOW_IDLE=0,BR_FLOW_NEXT=1,BR_FLOW_WAIT=2,
    BR_FLOW_DRAINING=3,BR_FLOW_DONE=4,BR_FLOW_FAILED=5,BR_FLOW_CANCELLED=6 };
typedef struct {
    enum br_flow_source source;
    const char *literal; /* encoded JSON scalar or complete row-object array */
    const char *step; /* preceding step id, JSON/TEXT only */
    const char *key; /* input key or declared top-level json field */
} br_flow_value;
typedef struct { const char *key; br_flow_value value; } br_flow_binding;
typedef struct {
    const char *id,*action,*target,*peer_op; /* peer_op: system.op only */
    const br_flow_binding *params,*set; size_t param_count,set_count;
    unsigned audio; /* requested open bits, default 0. Never READY authority. */
} br_flow_step;
typedef struct {
    const br_flow_step *steps; size_t count;
    const br_flow_value *output; /* NULL: last text, optional when absent */
} br_flow_definition;
typedef struct {
    void *context;
    /* Accepted request is borrowed until reaped. 0 busy/no side effects;
     * 1 accepted, -1 uncertain failure. No retry after acceptance/uncertainty.
     * Adapter MUST use operation_prepare, explicit session mode's
     * br_session_prepare, or system_submit + its local rules.
     * Sending OPEN exactly once and transport auth/heartbeats belong to owner. */
    int (*start)(void *,uint64_t ticket,uint64_t epoch,int local,const void *,size_t,
                 uint64_t now_ms,uint64_t remaining_ms);
    void (*cancel)(void *,uint64_t ticket,uint64_t now_ms);
    int (*reaped)(void *,uint64_t ticket,uint64_t now_ms); /* exactly 1 = released */
} br_flow_io;
typedef struct { uint16_t declarations,declarations_length,result,result_length;
    uint64_t epoch; char command[37]; } br_flow_record;
enum br_flow_checkpoint_event { BR_FLOW_PREPARE=1,BR_FLOW_SUCCESS,BR_FLOW_REAPED,BR_FLOW_NOT_SENT };
struct br_flow;
/* Synchronous non-reentrant durable barrier, exactly 1 commits. Snapshot is
 * borrowed during callback; no flow API calls or mutation, use own scratch.
 * PREPARE/NOT_SENT/SUCCESS index active slot; REAPED index after increment. */
typedef int (*br_flow_checkpoint)(void *,const struct br_flow *,unsigned event);
typedef struct br_flow {
    br_flow_io io;
    const br_flow_definition *definition;
    const br_flow_binding *inputs; size_t input_count;
    const char *script_id,*wake_source,*wake_text;
    uint64_t run,last_run,ticket,last_ticket,epoch,started_ms,step_ms,now_ms,timeout_ms;
    uint64_t session_epoch,session_generation; /* explicit opt-in, never audio authority */
    enum br_flow_status status,after_drain;
    const char *error; /* fixed local diagnostic or existing 14-code name */
    unsigned index,used; int local,oneway,acked,cancelled;
    int silent,deadline_expired; /* RAM-only original LOCAL clock proof, not error text */
    const br_system_rule *silent_rules; size_t silent_rule_count;
    char command[37],task[129];
    br_flow_record records[BR_FLOW_STEPS];
    unsigned char arena[BR_FLOW_ARENA],request[LX06_BRIDGE_PAYLOAD_MAX+1];
    size_t request_length;
    br_flow_checkpoint checkpoint; void *checkpoint_context;
} br_flow;
int br_flow_init(br_flow *,br_flow_io); /* fresh storage only */
/* Definitions/inputs/identity strings outlive run including drain. A enforces
 * input declarations, name NFC and configuration-wide limits before begin. */
int br_flow_begin(br_flow *,uint64_t run,uint64_t now,const br_flow_definition *,
                  const br_flow_binding *inputs,size_t count,const char *script_id,
                  const char *wake_source,const char *wake_text,br_json *scratch);
/* Before first dispatch, scheduled source only. One-way hardening: each phone
 * step requires silent caps + closed audio; each local step matching typed ACK
 * rule. Borrow rules unchanged through drain; backend still uses system_submit.
 * Cannot unset/change policy during a run. PROVISIONAL(P-20261111-1343-B). */
int br_flow_require_silent(br_flow *,uint64_t run,const br_system_rule *,size_t);
/* PROVISIONAL(P-20261111-2239-B): before first dispatch, pin one WSS identity.
 * Synchronous phone steps, requested audio closed, no local/oneway in API5.
 * Owner MUST use original br_session_prepare/receive with this generation;
 * only its current operation may be delivered. No IO or auth/audio proof.
 * Rejects an already installed generic checkpoint. An explicit fixed-session
 * identity barrier may be installed afterwards; old isolated mode unchanged.
 * 1 chosen, -1 refusal without mutation. Borrowed definitions stay immutable. */
int br_flow_use_session(br_flow *,uint64_t run,uint64_t epoch,uint64_t generation);
/* Once after begin before first dispatch; optional, old RAM API unchanged.
 * Bind to actual immutable definition/inputs and original schedule run.
 * Failed barrier stops; no automatic recovery/replay.
 * PROVISIONAL(P-20261111-1632-B). */
int br_flow_set_checkpoint(br_flow *,uint64_t run,br_flow_checkpoint,void *);
/* PROVISIONAL(P-20261112-0135-B): explicit fixed-session barrier only, after
 * use_session and before dispatch. Generic/scheduled checkpoint still refuses
 * this mode. Callback MUST validate the pinned identity and must not use the
 * legacy per-step-epoch journal. No recovery/replay permission. */
int br_flow_set_session_checkpoint(br_flow *,uint64_t run,br_flow_checkpoint,void *);
/* NEXT only. Validates refs + CURRENT caps, constructs complete request then
 * calls start. Owner supplies fresh ticket/UUID; legacy phone steps fresh
 * epoch, explicit session mode uses the pinned epoch/generation throughout.
 * Returns status; rejected preparation stops flow without dispatch. */
int br_flow_dispatch(br_flow *,uint64_t run,uint64_t ticket,uint64_t epoch,const char *command,
                     uint64_t now,const br_caps *,br_json *scratch);
/* Only original operation's ACK/READY/ERROR/CLOSED/TIMEOUT accepted. STATE,
 * TEXT, TASK and foreign/stale callbacks do not complete a step.
 * In session mode deliver only the current original session operation AFTER
 * br_session_receive checks command/task/generation; never bypass that router.
 * Route quota ERROR.msg to existing K5 BEFORE this call: cancel may invalidate
 * operation's borrowed response. Flow never rewrites or speaks that prose. */
int br_flow_operation(br_flow *,uint64_t run,uint64_t ticket,uint64_t now,
                      const br_operation *,int event,br_json *scratch);
int br_flow_system(br_flow *,uint64_t run,uint64_t ticket,uint64_t now,
                   const br_system *,br_json *scratch);
/* Callback: 1 = original association/version verified AND K6 receipt durable;
 * 0 = defer (keep waiting), -1 = reject. Row remains borrowed/private. */
typedef int (*br_flow_receipt)(void *,const br_json *,unsigned row);
int br_flow_task(br_flow *,uint64_t run,uint64_t ticket,uint64_t now,
                 const br_json *,unsigned row,br_flow_receipt,void *,br_json *scratch);
int br_flow_poll(br_flow *,uint64_t run,uint64_t now,int preempt,br_json *scratch);
/* JSON scalar output only after DONE, 1 present/0 absent/-1 invalid. Does not
 * play/log it. Failure leaves output length/data unchanged; scratch wiped. */
int br_flow_output(br_flow *,br_json *scratch,void *,size_t,size_t *length);
/* PROVISIONAL(P-20261111-1847-B): borrowed immutable historical success view.
 * No state machine/owner/receipt authority. Disjoint scratch/output/length and
 * immutable inputs; serial calls only. Records in declaration/result arena
 * order, count == definition count. No result copies or heap allocation. */
typedef struct {
    const br_flow_definition *definition;
    const br_flow_binding *inputs;size_t input_count;
    const br_flow_record *records[BR_FLOW_STEPS];
    const unsigned char *arena;unsigned used,count;
} br_flow_results;
/* Reuses live definition/reference/scalar rules. 1 JSON scalar (no NUL),
 * 0 absent default text, -1 invalid/overflow; out/length untouched on 0/-1.
 * Scratch erased on every path when non-NULL. No caps/IO/time/continuation. */
int br_flow_saved_output(const br_flow_results *,br_json *scratch,void *,size_t,size_t *length);
int br_flow_clear(br_flow *); /* only idle/terminal; erase private results */
#endif
