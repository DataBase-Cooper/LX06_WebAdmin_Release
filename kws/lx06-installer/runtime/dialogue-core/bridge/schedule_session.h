/* Local structural API1 plus isolated owning/success-retirement candidate.
 * PROVISIONAL(P-20261112-1540-B). No production attachment.
 * The comparison API alone performs no IO/callbacks/allocation/state changes.
 * Original serialized owner supplies immutable, disjoint borrowed objects.
 * A successful comparison is NOT durable association, caps/authentication,
 * current owner/resource proof, K6 receipt, send or recovery permission. */
#ifndef LX06_BR_SCHEDULE_SESSION_H
#define LX06_BR_SCHEDULE_SESSION_H
#include "flow_journal.h"
#include "flow_session_log.h"
#include "flow_owner_store.h"
#include "schedule_run.h"

/* Compare the original first-dispatch boundary: durable schedule INTENT,
 * freshly begun scheduled FLOW (silent policy already selected), and the two
 * typed bindings. command is the original aggregate UUID, exactly 37 bytes.
 * Every run/serial/config/output identity must agree; session ctl/cycle/job
 * must all be zero. No normalization, fallback, inferred IDs or retained view.
 * 1 = structural match only, -1 = refusal, with all inputs unchanged.
 * Only remote, closed-audio steps are supported by this contract. Mixed local
 * and oneway execution, three-store durable linkage, final aggregate receipt,
 * retirement and UNKNOWN recovery remain pending; use neither legacy journal
 * nor interactive retirement as substitutes. Old APIs retain their guards.
 * Inputs MUST come from original init/begin and stay immutable through call;
 * this is not a second definition/settings validator or a disk readback. */
int br_schedule_session_check(const br_schedule *,unsigned slot,const br_flow *,
    const br_flow_log_binding *,const br_flow_session_log_binding *,const char command[37]);
/* B30 owning prefix, serial/non-realtime. No schema migration. Borrow every
 * original object through UNKNOWN/drain. NEVER use legacy dispatch/interactive
 * retirement on this handle. Caller holds both A families before INTENT.
 * Durable INTENT -> original owner seed -> fixed-session BOUND -> installed
 * PREPARE -> scheduled owner append -> original fixed-session single send.
 * Full successful aggregate goes to original K6 BEFORE schedule TERMINAL.
 * Original schema9 never accepts scheduled tombstones. The separate explicit
 * success coordinator below writes schema10 only after the complete gate.
 * Keep A holds and all identities through uncertain writes and UNKNOWN.
 * No automatic release/replay/recovery; not a completed production entry. */
typedef struct {
    br_schedule_runner *runner;br_flow_session *adapter;
    br_flow_session_log *log;br_flow_owner_store *owner;
    br_flow_log_binding binding;char command[37];
    int attached,busy,receipt,retired,unsent_closing,deferred,failed;
} br_schedule_session;
/* AFTER original global claim, before session bind. All files genuinely
 * absent except original durable schedule INTENT; owner callbacks prove
 * current provenance, never unconditional admission in production. */
int br_schedule_session_attach(br_schedule_session *,br_schedule_runner *,
    br_flow_session *,br_flow_session_log *,br_flow_owner_store *,const br_flow_log_binding *,
    br_flow_owner_handoff_current,void *);
int br_schedule_session_dispatch(br_schedule_session *,uint64_t,uint64_t,uint64_t,const char *);
/* Consume the ORIGINAL session event returned by profile_pump ONCE, before
 * any other pump/poll erases its response. Not a frame parser/transport. */
int br_schedule_session_event(br_schedule_session *,uint64_t,uint64_t,int);
int br_schedule_session_poll(br_schedule_session *,uint64_t,uint64_t,int);
int br_schedule_session_sync(br_schedule_session *,uint64_t,br_inbox *,uint64_t,int,br_result *);
/* Readonly end gate: 1 real reaped, -1 missing evidence. Neither return grants
 * retirement or releases A/schedule. Use the separate explicit coordinator. */
int br_schedule_session_drained(br_schedule_session *,uint64_t,br_inbox *,br_result *);
/* Explicit scheduled SUCCESS retirement of this original live coordinator.
 * Rechecks original full owner/log prefix, exact on-disk schedule TERMINAL,
 * aggregate K6 receipt and CURRENT actual reap; then durably writes distinct
 * schema10/phase8 tombstone before releasing the original schedule owner.
 * Preserves identity in coordinator; clears runner only after durable success.
 * Does NOT release A sources, erase any file, permit tombstone replacement,
 * recover UNKNOWN, or create/consume sound. Source manager must separately
 * release its exact held origins. Old interactive retirement still refuses.
 * 1 retired and scheduler drained; -1 refusal/IO, all holds retained on IO. */
int br_schedule_session_retire(br_schedule_session *,uint64_t,br_inbox *,br_result *);
/* Only first step's empty BOUND on a pristine original fixed session. Checks
 * exact scheduler INTENT/zero-row owner before original NEXT cancellation and
 * original close request. Returns0 while CURRENT actual close/reap is pending.
 * Once reaped: distinct schema11 unsent tombstone -> original no-send backoff
 * (30/60/120/300s) -> exact readback/reap -> original scheduler release. No K6,
 * success, timeout, source release or retry admission. All identities/holds
 * survive ambiguity; reopen observes only. auth=1 pauses via original policy.
 * Returns1 deferred, -1 refusal/IO. Caller must separately release A sources. */
int br_schedule_session_defer_unsent(br_schedule_session *,uint64_t,uint64_t,int auth);
/* Original CURRENT terminal ERROR event, consumed exactly once before another
 * pump clears its body. Original code/msg -> original aggregate local K6,
 * original scheduler TERMINAL -> distinct failure witness. No phone identity
 * is fabricated; original per-step identity remains in the fixed prefix.
 * Called inside the original profile event borrow; never reenters profile.
 * Session close callback must remain the original nonreentrant close request.
 * Uses original FLOW cancellation; profile performs original ERROR cleanup.
 * Call failure_poll from the outer owner AFTER pump returns for global close
 * request/drain. Never call that poll inside the profile event borrow.
 * write uncertainty remains blocked/owned. No local timeout or UNKNOWN entry.
 * Subsequent explicit failure poll drains only, never dispatches. Retirement
 * requires actual closed/unowned resources and current original failure K6.
 * All objects borrowed through drain; no playback/production authorization. */
int br_schedule_session_failure(br_schedule_session *,uint64_t,uint64_t,br_inbox *,uint64_t,int,br_result *);
int br_schedule_session_failure_poll(br_schedule_session *,uint64_t,uint64_t);
int br_schedule_session_retire_failure(br_schedule_session *,uint64_t,br_inbox *,br_result *);
/* Read current old phase8/complete owner/K6/real closure. terminal=1 ALSO
 * compares the retained unowned TERMINAL before original calendar polling;
 * terminal=0 is prefix observation only after that original poll transition.
 * Neither mode grants new claim/send, reconstructs a live handoff or releases. */
int br_schedule_session_retired_inspect(br_schedule_session *,br_inbox *,br_result *,
    br_flow_session_log_closed,void *,int terminal);
/* AFTER a separately proven original NEW calendar claim. All IDs increase.
 * Exact old phase8/full owner/K6/current old reap precede new BOUND+new owner;
 * old coordinator identity survives independent of the now-new runner record.
 * Original idle runner wire is a temporary old-owner snapshot; no heap/image
 * store/schema is added. Any ambiguous publication keeps new holds/UNKNOWN. */
int br_schedule_session_turnover(br_schedule_session *,br_schedule_session *old,
    br_schedule_runner *,br_flow_session *,br_flow_session_log *,br_flow_owner_store *,
    const br_flow_log_binding *,const br_flow_session_log_binding *,br_inbox *,br_result *,
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Explicit live no-send successor. No K6 or success proof is invented: old
 * schema11/empty owner plus CURRENT old resource closure and original PENDING.
 * pending=1 requires exact retained old scheduler identity, before NEW claim;
 * pending=0 observes the old fixed prefix after original calendar checking.
 * New intent/identity/idle/caps/backoff remain original caller obligations. */
int br_schedule_session_deferred_inspect(br_schedule_session *,br_flow_session_log_closed,void *,int pending);
int br_schedule_session_retry(br_schedule_session *,br_schedule_session *old,
    br_schedule_runner *,br_flow_session *,br_flow_session_log *,br_flow_owner_store *,
    const br_flow_log_binding *,const br_flow_session_log_binding *,
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Next original calendar occurrence after confirmed failed retirement only.
 * Exact phase11/partial owner/original failure K6/current actual closure;
 * terminal=1 additionally proves retained old unowned TERMINAL before poll.
 * Separate new INTENT/four increasing identities precede ordered BOUND/owner.
 * No replay, readonly reconstruction, timeout or UNKNOWN admission. */
int br_schedule_session_failure_retired_inspect(br_schedule_session *,br_inbox *,br_result *,
    br_flow_session_log_closed,void *,int terminal);
int br_schedule_session_after_failure(br_schedule_session *,br_schedule_session *old,
    br_schedule_runner *,br_flow_session *,br_flow_session_log *,br_flow_owner_store *,
    const br_flow_log_binding *,const br_flow_session_log_binding *,br_inbox *,br_result *,
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Current original ACK-bound TASK -> durable exact per-step association only.
 * Call inside original TASK response borrow, before another poll clears it.
 * Never fabricates terminal/result/versions, accepts no caller identity, and
 * creates no K6 row. Uses the same retained session/log/scheduler owner with
 * full original response and caps fingerprints across durable boundaries.
 * No readonly/recovery, UNKNOWN retirement, replay, sound or next-step grant. */
int br_schedule_session_associate_task(br_schedule_session *,uint64_t,br_result *);
#endif
