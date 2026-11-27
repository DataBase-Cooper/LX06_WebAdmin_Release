/* LX06_TEST_FIXTURE_ONLY: A13 -> B05, until retained-source/T14 acceptance.
 * A's fixed object header is supplied by its owner, never copied into B.
 * No settings parser or execution permission from eo_load/describe alone.
 * Serial, disjoint heap/caller storage; keep this definition through drain.
 */
#ifndef LX06_BR_OBJECT_FLOW_H
#define LX06_BR_OBJECT_FLOW_H
#ifndef LX06_EXTENSION_OBJECTS_FIXTURE
#error "A object consumers have no production registration"
#endif
#include "app-extension-objects.h"
#include "flow.h"
#include "flow_owner_store.h"
enum br_object_rc { BR_OBJECT_OK=1, BR_OBJECT_INVALID=-1,
    BR_OBJECT_UNSUPPORTED=-2, BR_OBJECT_UNVERIFIED=-3 };
typedef struct {
    br_flow_definition definition;
    br_flow_step steps[BR_FLOW_STEPS];
    br_flow_binding params[BR_FLOW_STEPS][16],set[BR_FLOW_STEPS][16];
    br_flow_value output;
    struct eo_origin origin;
    unsigned used;
    char arena[BR_FLOW_ARENA];
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
    br_flow_binding inputs[16];size_t input_count;
    int inputs_verified;char inputs_sha256[65];
#endif
} br_object_flow;
/* Fresh/unborrowed output only; failed conversion erases output. All node,
 * key/string/ref pointers are copied within eo_consume. No A pointer survives.
 * Current subset: basic scalar/text steps without inputs/enhanced policies.
 * json_fields has names only: JSON result refs remain UNVERIFIED until actual
 * typed-return evidence exists. Complete scalar-row arrays use the existing
 * bounded literal/input carrier; enhanced policies remain UNSUPPORTED.
 * Original br_flow_begin/dispatch still validate CURRENT caps per step.
 */
int br_object_flow_copy(br_object_flow *,struct eo_owner *,const struct eo_origin *,const char *name);
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
/* Only A's current private preparation can authorize input.KEY. The old copy,
 * start and new-intent APIs continue rejecting nonempty declarations. */
int br_object_flow_copy_prepared(br_object_flow *,struct eo_owner *,const struct eo_origin *,const char *);
#endif
/* Typed scalar -> encoded JSON in bounded caller buffer, not a JSON parser.
 * Used by the two original-object consumers; output untouched on failure. */
int br_object_scalar(const struct eo_view *,int,char *,size_t,size_t *);
/* Candidate composition of the ORIGINAL owners, no registry/receipt synthesis.
 * Caller supplies a saved-source lease and original current provenance. All
 * handles and callback contexts outlive drain/recovery. Never reinit a live
 * run. A failed durable prefix retains its source, even when start returns -1.
 * No scheduled, audio or enhanced-policy admission is added here. */
typedef int (*br_object_source_current)(void *,const struct eo_origin *);
struct selected_script_profile;
typedef struct {
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
    struct selected_script_profile *selected;
#endif
    br_object_flow *object;struct eo_owner *source;struct eo_origin origin;
    br_flow_session *adapter;br_flow_session_log *log;br_flow_owner_store *owner;
    br_object_source_current current;void *context;
    int held,busy,result_handed_off;
} br_object_flow_run;
int br_object_flow_start(br_object_flow_run *,br_object_flow *,struct eo_owner *,
    const struct eo_origin *,const char *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,uint64_t now,br_object_source_current,void *,
    br_flow_owner_handoff_current previous,void *);
int br_object_flow_dispatch(br_object_flow_run *,uint64_t run,uint64_t ticket,
    uint64_t now,const char *command);
int br_object_flow_event(br_object_flow_run *,uint64_t run,uint64_t ticket,
    uint64_t now,int event);
int br_object_flow_poll(br_object_flow_run *,uint64_t run,uint64_t now,int preempt);
/* Fixed diagnostic only, never terminal/receipt/release authority. */
const char *br_object_flow_outcome(const br_object_flow_run *);
/* One synchronous whole-flow result in the UNIQUE original K6 inbox. Uses
 * original FLOW output and original inbox association/encoding; no fake TASK.
 * identity/progress are private typed observations, not release receipts. */
int br_object_flow_result(br_object_flow_run *,br_inbox *,uint64_t wall_ms,int trusted,
    br_result *identity,br_result_progress *);
/* Actual original closed-resource/CTL/result-handoff proof remains mandatory.
 * Failed/cancelled/timeout/UNKNOWN and any unproven obligation retain source.
 * A historical tombstone alone never authorizes release. */
int br_object_flow_retire(br_object_flow_run *,br_flow_session_log_closed,void *);
/* Explicit new intent AFTER real retirement/readback. Fresh/cleared original
 * adapter on a new live profile session; all four identities must increase.
 * Original old/new provenance and CURRENT old closed proof stay mandatory.
 * Existing bytes never disappear on refusal or ambiguous publication. */
int br_object_flow_new_intent(br_object_flow_run *,br_object_flow *,struct eo_owner *,
    const struct eo_origin *,const char *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,const br_flow_session_log_binding *,uint64_t now,
    br_object_source_current,void *,br_flow_owner_handoff_current,void *,
    br_flow_session_log_closed,void *);
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
int br_object_flow_start_prepared(br_object_flow_run *,br_object_flow *,struct eo_owner *,
    const struct eo_origin *,const char *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,uint64_t,br_object_source_current,void *,br_flow_owner_handoff_current,void *);
#ifdef LX06_SELECTED_FLOW_PROFILE_FIXTURE
/* Explicit source-owned transfer from the ORIGINAL selected profile. The
 * ordinary start APIs still require a fresh eo_hold and refuse a double hold. */
int br_object_flow_start_prepared_selected(br_object_flow_run *,br_object_flow *,struct eo_owner *,
    const struct eo_origin *,const char *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,uint64_t,br_object_source_current,void *,br_flow_owner_handoff_current,void *,
    struct selected_script_profile *);
#endif
int br_object_flow_new_intent_prepared(br_object_flow_run *,br_object_flow *,struct eo_owner *,
    const struct eo_origin *,const char *,br_flow_session *,br_flow_session_log *,
    br_flow_owner_store *,const br_flow_session_log_binding *,uint64_t,
    br_object_source_current,void *,br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *);
#endif
#endif
