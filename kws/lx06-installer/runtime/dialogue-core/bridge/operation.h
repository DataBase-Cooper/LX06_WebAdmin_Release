/* T16 isolated business control consumer, local API3.
 * PROVISIONAL(P-20261111-1207-B/P-20261112-2025-B). No sockets, audio IO or production wiring.
 * Allocate off the real-time stack. Calls are serial and buffers disjoint.
 */
#ifndef LX06_BR_OPERATION_H
#define LX06_BR_OPERATION_H
#include "discovery.h"
enum br_operation_event {
    BR_OP_NONE=0,BR_OP_PREPARED=1,BR_OP_ACK=2,BR_OP_READY=3,BR_OP_TASK=4,
    BR_OP_PROGRESS=5,BR_OP_DIAGNOSTIC=6,BR_OP_HEARTBEAT=7,BR_OP_TASKS=8,
    BR_OP_INVALID=-1,BR_OP_CLOSED=-2,BR_OP_TIMEOUT=-3,BR_OP_ERROR=-4
};
typedef struct {
    br_discovery *discovery;
    br_json response; /* event data only, NEVER caps/readiness authority */
    br_caps_plan plan; /* borrowed caps views; invalid after connection close */
    uint64_t epoch,started_ms,timeout_ms;
    char command_id[37],task_id[129];
    const char *error; /* fixed static name, never received prose */
    unsigned confirmed_audio;
    int pending,acked,completed,connecting,list_mode,last_event;
} br_operation;
/* discovery must outlive operation. Do not replace/mutate its caps or send
 * describe/business requests concurrently. Each discovery epoch permits only
 * ONE business OPEN, even across operation re-init. Owner chooses fresh epoch.
 */
void br_operation_init(br_operation *,br_discovery *);
void br_operation_clear(br_operation *); /* erases local state, no IO */
/* Local API3: shared GRANT semantic check on an already parsed strict object.
 * Exact command, nonempty task (1..128B) and state=started; bound_task may be
 * NULL before binding, otherwise must match. Pure validation, no ACK/terminal,
 * caps, generation, epoch or resource authority. Caller validates Wire first. */
int br_operation_grant_check(const br_json *,const char *command,const char *bound_task);
/* Pure shared encoder (local API2, P-20261111-2004-B): validates the same
 * caps/envelope and returns borrowed plan + copied UUID only on success.
 * No discovery/operation mutation, send/readiness/serial reuse authority.
 * list=0 business or 1 task.list; generation=0 for legacy single request.
 * State, scratch, packet, outputs and input MUST be disjoint. */
int br_operation_encode(const br_caps *,uint64_t epoch,uint64_t generation,int list,
    const void *,size_t,br_json *,dc_wire_packet *,br_caps_plan *,char command_id[37],
    void *wire,size_t capacity,size_t *length);
/* Validates complete owner-resolved OPEN and encodes one binary Wire message.
 * Failure leaves output/length unchanged. On success SEND ONCE; send failure
 * MUST call abort. Preparing does not prove send/ACK/READY or confirm audio.
 */
int br_operation_prepare(br_operation *,uint64_t epoch,uint64_t now_ms,
                         const void *open,size_t,void *wire,size_t capacity,size_t *length);
/* P-1225: independent task.list, 5s, same one-request-per-epoch rule. READY
 * kind=tasks is transport completion only; br_delivery_accept must durably
 * admit original rows before claiming local receipt. No pagination. */
int br_operation_prepare_list(br_operation *,uint64_t epoch,uint64_t now_ms,
                              const void *open,size_t,void *wire,size_t capacity,size_t *length);
int br_operation_poll(br_operation *,uint64_t now_ms);
void br_operation_abort(br_operation *); /* clear connection authority */
/* Only business control events, PING/PONG and TASK; discovery setup remains
 * br_discovery's job. Unsupported AUDIO/RESUMED/etc close this control-only
 * consumer. TASK never completes pending: caller MUST pass original rows to
 * K6 and wait for durable association, including oneway ACK + disconnect.
 * response is private, valid until next prepare/receive/clear/abort; do not
 * log it. READY.confirmed_audio is independent of requested_audio; end/keep is
 * plan.session_end, never inferred from a silent/query audio vector.
 * M16 heartbeat events carry no response/authority: PING requests exact echo
 * by the sole transport owner using original bytes + br_wire_pong; only the
 * discovery's current matching PONG refreshes liveness. Other PONG drop=0.
 */
int br_operation_receive(br_operation *,uint64_t epoch,uint64_t now_ms,const void *,size_t);
#endif
