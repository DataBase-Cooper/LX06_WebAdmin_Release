/* LX06_TEST_FIXTURE_ONLY: A14 typed config -> original global scheduler.
 * Serial HOST candidate. Original store/runner/K6 only, no timer thread.
 */
#ifndef LX06_BR_OBJECT_SCHEDULE_H
#define LX06_BR_OBJECT_SCHEDULE_H
#include "object_flow.h"
#include "schedule_run.h"
#include "schedule_session.h"
typedef struct {
    br_schedule_task tasks[BR_SCHEDULE_TASKS];size_t count;
    struct { const char *action,*target,*params,*flow;int inputs; } run[BR_SCHEDULE_TASKS];
    struct eo_origin origin;
    char revision[65]; /* SHA256(script||carrier||settings||LE64 revision) */
    char arena[32768];unsigned used;
    int held;unsigned slot;
    uint64_t serial,epoch;char command[37];
} br_object_schedule;
/* Seeds are original owner entropy, supplied for all entries (disabled too).
 * init of original scheduler durably chooses/preserves seed before use.
 * Fresh/unborrowed output only; original config owns this until release.
 * Flow references are retained but never sent as phone action. Same-source
 * A13 resolution + fixed-session three-store adapter remain UNVERIFIED.
 */
int br_object_schedule_copy(br_object_schedule *,struct eo_owner *,const struct eo_origin *,const uint64_t seeds[32]);
/* Compatibility adapter for one registered script. Multi-script owners use
 * the complete retained-source registry below. Both use the same runner.
 * Call on every event-loop timer/clock/activity update. Rejected samples use
 * the ORIGINAL no-authority poll to invalidate candidates, subject to its
 * existing busy/reentry guards; start independently rechecks every sample.
 */
int br_object_schedule_poll(br_object_schedule *,struct eo_owner *,br_schedule_runner *,const struct eo_clock *,br_schedule_candidate *);
/* Reborrow/recheck the exact clock/source before hold -> original acquire ->
 * durable INTENT -> current caps -> single send. Caller supplies disjoint
 * 8193B OPEN buffer. Synchronous actions only in this consumer subset.
 */
int br_object_schedule_start(br_object_schedule *,struct eo_owner *,br_schedule_runner *,
    const struct eo_clock *,uint64_t run,uint64_t epoch,const char *uuid,char *open,size_t capacity);
/* Release A's held definition only after original TERMINAL, global real reap,
 * and matching durable K6 receipt. UNKNOWN/timeout notice cannot release.
 * scratch is disjoint caller-owned private result storage, wiped after call.
 * Original runner receive/sync/tick/recover and B08 observations remain APIs.
 */
int br_object_schedule_release(br_object_schedule *,struct eo_owner *,br_schedule_runner *,br_inbox *,br_result *scratch);
/* One immutable binding per slot of the ORIGINAL complete runner registry.
 * Every script is included, even empty/disabled/unreconciled ones. The caller
 * enumerates the complete device config; this adapter does not discover files.
 * A mode=2 saved source is mandatory. No supplied digest substitutes for its
 * retained directories/files/settings lock. Borrow all storage until reset.
 * Initialize zeroed, unregistered storage only. Reset only after no candidate,
 * active/unknown/owned run, or held A definition remains; reconfigure the
 * original schedules and runner, then register again. No in-place edits.
 */
struct eo_saved_source;
struct eo_schedule_directory;
typedef struct {
    br_object_schedule *object;struct eo_owner *owner;br_schedule *schedule;
} br_object_schedule_binding;
typedef struct {
    br_schedule_runner *runner;size_t count;int ready,busy;
    /* Optional immutable A scope, installed by the directory adapter before
     * publication. Never mutate these callbacks while ready. The scope adds
     * a refusal gate; it cannot replace any original source/runner check. */
    void *scope;int (*scope_current)(void *);int (*scope_release)(void *);
    struct {
        br_object_schedule_binding binding;
        struct eo_saved_source *source;
        struct eo_origin origin;char script[129],revision[65];
    } entries[BR_SCHEDULE_TASKS];
} br_object_schedule_registry;
int br_object_schedule_registry_init(br_object_schedule_registry *,br_schedule_runner *,
    const br_object_schedule_binding *,size_t);
/* Complete A directory consumer: bindings must exactly match every nonempty
 * A source in its original order and script name. Empty/absent definitions
 * remain in the directory snapshot. Every registry gate also rechecks newly
 * added files, names, directory replacements and empty-carrier changes.
 * The registry borrows this snapshot until successful quiescent reset. */
int br_object_schedule_registry_init_directory(br_object_schedule_registry *,br_schedule_runner *,
    const br_object_schedule_binding *,size_t,struct eo_schedule_directory *);
int br_object_schedule_registry_reset(br_object_schedule_registry *);
/* Revalidate ALL sources before/after the original global poll, including
 * disabled and nonselected scripts. A refused sample invalidates the previous
 * candidate via the original no-authority poll. No second scheduling policy. */
int br_object_schedule_registry_poll(br_object_schedule_registry *,const struct eo_clock *,br_schedule_candidate *);
/* Resample/revalidate before selecting and again after atomic owner acquire
 * and before send. The original acquire/send no-reentry contract still applies.
 * Source loss after acquire preserves original close/drain/UNKNOWN obligations.
 * Only the selected source is held; no new journal, scheduler or idle owner. */
int br_object_schedule_registry_start(br_object_schedule_registry *,const struct eo_clock *,
    uint64_t,uint64_t,const char *,char *,size_t);
/* Retire the selected original slot only after TERM/K6/reap, even if an
 * unrelated source was externally replaced. Never authorizes a fresh send. */
int br_object_schedule_registry_release(br_object_schedule_registry *,unsigned,br_inbox *,br_result *);
/* B30 actual fixed-session prefix, two independent A families. Caller opens
 * original absent log using exact binding, current-owner callback and store;
 * binding flow_sha256=FLOW carrier, definition_sha256=SHA256(carrier||name),
 * inputs_sha256=SHA256("{}"), revision=original SCHEDULE composite digest.
 * No inputs (even an explicit empty inputs object) in this subset. Keep copied
 * definition and BOTH holds through unresolved retirement. Frozen schema9
 * has no scheduled retire; this entry cannot release either A family. */
int br_object_schedule_start_session(br_object_schedule *,struct eo_owner *,
    struct eo_owner *,const struct eo_origin *,br_object_flow *,br_schedule_runner *,
    const struct eo_clock *,br_schedule_session *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,uint64_t,uint64_t,const char *,br_flow_owner_handoff_current,void *);
/* Real Linux time source into the SAME original clock/sample APIs. No
 * timer, registration, dispatch policy or idle authority is introduced.
 * Poll and start each resample; losing synchronization invalidates a prior
 * candidate through the original no-authority path. Existing explicit-sample
 * calls remain unchanged for their owners. */
#include "../../common/system-clock.h"
static inline int br_object_schedule_poll_system(br_object_schedule *o,struct eo_owner *owner,
    br_schedule_runner *r,struct lx06_system_clock *source,int idle,br_schedule_candidate *out) {
    struct eo_clock clock={0,idle,0,0};
    clock.trusted=lx06_system_clock_read(source,&clock.wall_ms,&clock.mono_ms)==1;
    return br_object_schedule_poll(o,owner,r,&clock,out);
}
static inline int br_object_schedule_start_system(br_object_schedule *o,struct eo_owner *owner,
    br_schedule_runner *r,struct lx06_system_clock *source,int idle,uint64_t run,uint64_t epoch,
    const char *uuid,char *open,size_t capacity) {
    struct eo_clock clock={0,idle,0,0};
    clock.trusted=lx06_system_clock_read(source,&clock.wall_ms,&clock.mono_ms)==1;
    return br_object_schedule_start(o,owner,r,&clock,run,epoch,uuid,open,capacity);
}
#endif
