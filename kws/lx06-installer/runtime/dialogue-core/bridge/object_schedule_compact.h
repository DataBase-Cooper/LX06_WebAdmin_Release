/* Compact A14 consumer of the ORIGINAL B global schedule runner.
 * No second scheduler, state file, idle owner or production registration. */
#ifndef LX06_BR_OBJECT_SCHEDULE_COMPACT_H
#define LX06_BR_OBJECT_SCHEDULE_COMPACT_H
#include "object_schedule.h"
#include "schedule_state_owner.h"
#include "../../../hooks/webd/app-schedule-compact.h"
#include "../../../hooks/webd/app-schedule-compact-directory.h"
typedef struct {
    void *context;
    int (*current)(void *); /* complete A directory scope, including empties */
    int (*retain)(void *);
    int (*release)(void *);
} br_compact_schedule_scope;
struct br_compact_schedule_session;
typedef struct {
    br_schedule_task tasks[BR_SCHEDULE_TASKS];
    struct eo_schedule_run_ref runs[BR_SCHEDULE_TASKS];
    struct { struct eo_compact_schedule_source *source;br_schedule *schedule;unsigned first,count; } scripts[BR_SCHEDULE_TASKS];
    size_t count,task_count;union eo_compact_schedule_work *work;
    struct eo_compact_schedule_root *root;
    struct eo_compact_schedule_directory *directory;
    br_schedule_runner *runner;br_compact_schedule_scope scope;
    br_schedule_state_owner *state_owner;
    int ready,busy,held,closing;unsigned held_script,held_slot;
    uint64_t serial,epoch;char command[37];
    struct br_compact_schedule_session *session; /* explicit prepared/owned FLOW */
} br_compact_schedule;
/* Caller zeroes a fresh set and initializes the separate reusable A workspace.
 * Each call stages one complete original source using A's typed callback;
 * failure erases only the unpublished tail. 1 nonempty source, 0 valid empty
 * source, -1 refusal. Empty source is closed after validation; the complete
 * A directory scope must retain/validate its existence and bytes.
 * Seeds are original owner entropy for all tasks, disabled included. */
int br_compact_schedule_load(br_compact_schedule *,union eo_compact_schedule_work *,
    struct eo_compact_schedule_root *,const char *script,uint64_t revision,
    const uint64_t seeds[32],eo_nfc,void *);
/* Complete directory loader for a fresh compact set. The original root is
 * caller owned and may be shared with a quiescent staged old generation.
 * All carriers, including empty ones, go through the actual A parser. */
typedef int (*br_compact_schedule_seeds)(void *,const char *script,uint64_t out[32]);
int br_compact_schedule_load_directory(br_compact_schedule *,union eo_compact_schedule_work *,
    struct eo_compact_schedule_root *,uint64_t revision,br_compact_schedule_seeds,void *,
    eo_nfc,void *,struct eo_compact_schedule_directory **out);
/* Borrow exact config to initialize ORIGINAL schedule/state objects. Revision
 * is the same SHA256(script||carrier||settings||LE64 generation) as legacy.
 * Caller never allocates another copied task array or per-script run arena. */
int br_compact_schedule_config(const br_compact_schedule *,size_t,const char **script,
    char revision[65],const br_schedule_task **tasks,size_t *count);
int br_compact_schedule_bind(br_compact_schedule *,br_schedule_runner *,br_compact_schedule_scope);
int br_compact_schedule_bind_directory(br_compact_schedule *,br_schedule_runner *);
int br_compact_schedule_bind_state_owner(br_compact_schedule *,br_schedule_state_owner *);
int br_compact_schedule_poll(br_compact_schedule *,const struct eo_clock *,br_schedule_candidate *);
int br_compact_schedule_start(br_compact_schedule *,const struct eo_clock *,
    uint64_t run,uint64_t epoch,const char *uuid,char *open,size_t capacity);
int br_compact_schedule_release(br_compact_schedule *,br_inbox *,br_result *scratch);
/* Unbind requires original quiescence/candidate invalidation/K6/UNKNOWN gates.
 * It preserves copied task bytes because original schedule structs may still
 * borrow them for a staged original reconfigure. Close only after the owner
 * has moved all original schedule references away from this set. */
int br_compact_schedule_unbind(br_compact_schedule *);
int br_compact_schedule_close(br_compact_schedule *);
size_t br_compact_schedule_bytes(void);
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
/* Original fixed-session scheduler consumer. Every object is borrowed through
 * UNKNOWN and actual drain. No new parser, dispatcher, log or queue; explicit
 * phase-separated codecs remain in the original single fixed-session log.
 * Full original A view/input allocations still require resource acceptance. */
typedef struct br_compact_schedule_session {
    br_compact_schedule *set;struct eo_saved_source *source;struct eo_owner *owner;
    br_object_flow *object;br_flow_session *adapter;struct eo_origin origin;
    br_schedule_session coordinator;br_schedule_candidate selected;
    br_flow_session_io original_io;
    uint64_t run,session_run,epoch,generation;
    char name[129];int busy,prepared,begun,held,started,released;
    struct br_compact_schedule_session *predecessor;
    uint64_t retired_sequence,successor_run;int32_t retired_day;int calendar_advanced;
} br_compact_schedule_session;
/* Fresh zero handle, empty original saved source/owner and unborrowed object.
 * Current global candidate chooses the full actual script; sources, named
 * FLOW and complete inputs are obtained through A, then copied by original B.
 * Return exact binding for original br_flow_session_log_open (absent only).
 * inputs is a disjoint reusable >=8193B buffer, always erased before return.
 * No acquire/INTENT/send. Explicit discard is available only before claim. */
int br_compact_schedule_session_prepare(br_compact_schedule_session *,br_compact_schedule *,const struct eo_clock *,
    struct eo_saved_source *,struct eo_owner *,br_object_flow *,br_flow_session *,uint64_t run,uint64_t generation,
    int flow_directory,int flow_parent,const char *flow_entry,br_flow_session_log_binding *,char *inputs,size_t);
int br_compact_schedule_session_discard(br_compact_schedule_session *);
/* Exact prepared binding, original absent log/owner store, genuine previous
 * owner proof. Current full directory and BOTH sources rechecked after global
 * atomic acquire and immediately before each actual fixed-session send.
 * Any uncertain committed prefix keeps the actual holds and original IDs. */
int br_compact_schedule_session_start(br_compact_schedule_session *,const struct eo_clock *,br_flow_session_log *,
    br_flow_owner_store *,const char *command,br_flow_owner_handoff_current,void *);
int br_compact_schedule_session_dispatch(br_compact_schedule_session *,uint64_t ticket,uint64_t now,const char *command);
int br_compact_schedule_session_event(br_compact_schedule_session *,uint64_t now,int event);
int br_compact_schedule_session_poll(br_compact_schedule_session *,uint64_t now,int preempt);
int br_compact_schedule_session_sync(br_compact_schedule_session *,br_inbox *,uint64_t wall,int trusted,br_result *);
/* Original success tombstone coordinator first, then original schedule and
 * FLOW source release with exact retained identity/K6/current-reap proofs.
 * Failure/UNKNOWN never releases. Partial release stays pinned to this set
 * until all original obligations finish. No tombstone reuse/new intent. */
int br_compact_schedule_session_retire(br_compact_schedule_session *,br_inbox *,br_result *);
/* Explicit zero-send deferral. Empty original BOUND cancellation and actual
 * close/reap precede distinct unsent tombstone and original scheduler backoff.
 * Releases both exact A holds only after current original proof. Partial
 * release stays pinned. 1 fully deferred/released, 0 closing, -1 refusal/IO.
 * No K6 receipt, retry admission or recovered continuation. */
int br_compact_schedule_session_defer_unsent(br_compact_schedule_session *,uint64_t,int auth);
/* Consume one CURRENT real original session ERROR into original aggregate
 * K6/terminal/failure witness, then original cancellation and actual drain.
 * Code/msg retain their original content and scheduled silent policy; remote
 * TASK/versions are never fabricated. No local timeout or UNKNOWN admission.
 * Separate poll and failure retirement keep both A holds until actual closure.
 * The old success retirement/turnover APIs never accept failure tombstones. */
int br_compact_schedule_session_failure(br_compact_schedule_session *,uint64_t,br_inbox *,uint64_t,int,br_result *);
int br_compact_schedule_session_failure_poll(br_compact_schedule_session *,uint64_t);
int br_compact_schedule_session_retire_failure(br_compact_schedule_session *,br_inbox *,br_result *);
/* Explicit live successor staging. Checks old durable TERMINAL before the
 * original poll can advance it, then keeps the original in-memory handoff
 * pinned to the exact retired coordinator and unchanged calendar sequence.
 * Rechecks phase8/owner/K6/CURRENT old resource closure on every attempt.
 * This handle cannot be recreated by readonly reopen or a cached receipt. */
int br_compact_schedule_session_prepare_next(br_compact_schedule_session *,br_compact_schedule_session *old,
    const struct eo_clock *,struct eo_saved_source *,struct eo_owner *,br_object_flow *,br_flow_session *,uint64_t,uint64_t,
    int,int,const char *,br_flow_session_log_binding *,char *,size_t,br_inbox *,br_result *,br_flow_session_log_closed,void *);
/* Strict new run/session/epoch/generation. Original claim -> inspected old
 * fixed prefix/current closure -> durable BOUND -> durable owner -> dispatch.
 * Both files remain untouched on early refusal; ambiguous prefixes stay held.
 * previous validates the FULL old owner bytes, never absence. */
int br_compact_schedule_session_start_next(br_compact_schedule_session *,const struct eo_clock *,const char *,
    br_inbox *,br_result *,br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Explicit no-send retry only, original live deferred predecessor. Exact
 * phase9/empty owner/current closure and retained PENDING before original
 * backoff/grace/auth/idle calendar poll. New claim gets a fresh serial and all
 * four run/session/epoch/generation identities increase. No K6 fabricated;
 * reopened state cannot recreate this live predecessor or auto-resume. */
int br_compact_schedule_session_prepare_retry(br_compact_schedule_session *,br_compact_schedule_session *old,
    const struct eo_clock *,struct eo_saved_source *,struct eo_owner *,br_object_flow *,br_flow_session *,uint64_t,uint64_t,
    int,int,const char *,br_flow_session_log_binding *,char *,size_t,br_flow_session_log_closed,void *);
int br_compact_schedule_session_start_retry(br_compact_schedule_session *,const struct eo_clock *,const char *,
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Explicit next original calendar occurrence after confirmed failure. Current
 * phase11/partial owner/failure K6/actual old closure before original calendar
 * transition, then separate NEW INTENT and four strictly increasing IDs.
 * No automatic retry, readonly reconstruction or UNKNOWN admission; original
 * successful/unsent successor APIs continue to reject this failed phase. */
int br_compact_schedule_session_prepare_after_failure(br_compact_schedule_session *,br_compact_schedule_session *old,
    const struct eo_clock *,struct eo_saved_source *,struct eo_owner *,br_object_flow *,br_flow_session *,uint64_t,uint64_t,
    int,int,const char *,br_flow_session_log_binding *,char *,size_t,br_inbox *,br_result *,br_flow_session_log_closed,void *);
int br_compact_schedule_session_start_after_failure(br_compact_schedule_session *,const struct eo_clock *,const char *,
    br_inbox *,br_result *,br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
/* Original current TASK borrow only; preserve both A holds and all scheduling
 * obligations. Durably binds only the exact original ACK task/app/hook to the
 * current prepared row; no delivery/terminalization or UNKNOWN resolution. */
int br_compact_schedule_session_associate_task(br_compact_schedule_session *,br_result *);

#endif
#endif
