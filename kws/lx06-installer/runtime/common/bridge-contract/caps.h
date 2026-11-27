/* K3/T17 shared C consumer, paired with caps.schema.json + validate.py.
 * No dialer, settings parser, matcher, phone service or snapshot persistence.
 * Isolated until T14; callers supply authenticated READY payloads only.
 * Local C API 8: official rev13 WFP-1-A bounded array/items.object
 * descriptors in current live caps and separate save-only snapshots. Scalar
 * item fields only; required/optional match original scalar params. No heap
 * or new wire versions. Original bounds: PROVISIONAL(P-20261111-1025-B).
 */
#ifndef LX06_BR_CAPS_H
#define LX06_BR_CAPS_H
#include "caps_json.h"
enum br_caps_rc { BR_CAPS_OK=1, BR_CAPS_INVALID=-1, BR_CAPS_VERSION=-2,
    BR_CAPS_HOOK=-3, BR_CAPS_APP_VERSION=-4, BR_CAPS_APP_MISSING=-5,
    BR_CAPS_OFFLINE=-6 };
enum br_caps_audio { BR_CAPS_MIC=1, BR_CAPS_SPEAKER=2, BR_CAPS_RING=4 };
typedef struct { br_json document; uint64_t connection; int ready; } br_caps;
typedef struct {
    br_text app, op, end_op, connection_op, ready_signal;
    uint64_t timeout_ms, connection_timeout_ms;
    unsigned requested_audio; /* requested open bits, NEVER READY confirmation */
    int session_end, oneway, asynchronous, silent, degraded;
    int cancel_mode; /* 0 undeclared, 1 pause_hold, 2 freeze_only */
} br_caps_plan;
/* Caller allocates state/scratch outside the audio callback stack, serializes
 * calls, and treats documents as opaque. All pointer arguments must be disjoint.
 * begin is mandatory for EVERY connection/caps request, with a new nonzero
 * connection identity (normally epoch); it destroys prior runtime authority.
 * forget on disconnect, token/auth failure, heartbeat death or preemption.
 */
void br_caps_begin(br_caps *,uint64_t connection);
void br_caps_forget(br_caps *);
/* Only READY(32) kind=caps can establish authority. Refusal also clears it.
 * This is schema validation, not proof of TLS or operation readiness.
 */
int br_caps_accept(br_caps *,uint64_t connection,unsigned frame,const void *,size_t);
/* For save-time validation only: caller owns the token-free local snapshot.
 * The result document cannot be installed as runtime authority by an API.
 */
int br_caps_validate(br_json *,const void *,size_t);
/* Official rev13 ONLINE schema: original scalars plus explicitly declared
 * bounded arrays of complete scalar rows, including peer declarations.
 * Validation alone grants no authority; accept still requires current READY.
 * Save snapshots cannot replace that connection. Refusal wipes the document;
 * uses existing unsupported, no feature flag, new frame or Wire version. */
int br_caps_validate_live(br_json *,const void *,size_t);
/* Resolved request object {app,op,params?,set?,audio?}; optional objects default
 * to {}. Parameters are already resolved by A's sole parser/matcher. Neither
 * natural language nor script source enters this interface. No IO is done.
 * scratch is wiped after parsing. Plan unchanged on failure; successful string
 * views borrow caps until the next begin/accept/forget (do not retain them).
 */
int br_caps_operation(const br_caps *,uint64_t,const void *,size_t,br_json *scratch,br_caps_plan *);
/* Full business OPEN envelope (rev9 6.3), already resolved by A. Exact
 * <app>.<op> lookup, target defaults to app; no system/flow authorization.
 * Same plan/scratch rules. PROVISIONAL(P-20261111-1207-B). */
int br_caps_open_operation(const br_caps *,uint64_t,const void *,size_t,br_json *scratch,br_caps_plan *);
/* Full task.list OPEN, no target/audio/set, params absent or {}. Uses the
 * same envelope validator; no hook op/feature grants system authority.
 * PROVISIONAL(P-20261111-1225-B). scratch wiped, no IO. */
int br_caps_task_list_request(const br_caps *,uint64_t,const void *,size_t,br_json *scratch);
/* Local API9: complete caps.request OPEN using the SAME strict envelope rules.
 * Before authentication this validates data only, never installs authority.
 * target absent/system, params absent/{}, no audio/set. Scratch always erased.
 * Serial disjoint buffers; no settings/script parser or protocol version bump. */
int br_caps_discovery_request(uint64_t,const void *,size_t,br_json *scratch);
int br_caps_save_operation(const br_json *snapshot,const void *,size_t,br_json *scratch,br_caps_plan *);
/* Save-only {app,op,params:<caps-style declarations>}. A supplies the final
 * mapped parameter schema after its sole preparation chain; no script/input
 * DSL, defaults, list selection or mapping is parsed here. Proves a same-type
 * subset of snapshot params (required/optional, ranges, enum, complete rows).
 * Missing params defaults to {}. Incompatible/unknown keys/types refuse;
 * cross-type coercions refuse even when some values could be accepted.
 * No plan/runtime authority, IO or persistence. Original 8192B JSON limit.
 * snapshot is revalidated, never modified; disjoint scratch wiped on return.
 * Snapshot was obtained without credentials; caller serializes borrowed input.
 */
int br_caps_save_schema(const br_json *snapshot,const void *,size_t,br_json *scratch);
/* Save-only peer variant {op,params?}, resolving caps.peer_ops exclusively.
 * Same subset/snapshot/scratch rules; app/action/target/audio/set are refused.
 * No local operation or path permission: original br_system_submit and typed
 * owner allowlist must still admit every execution using current live caps.
 */
int br_caps_save_peer_schema(const br_json *snapshot,const void *,size_t,br_json *scratch);
/* Save-only {op,params?} with concrete constants from A's preparation chain.
 * Uses the original peer value validator, including complete bounded rows.
 * Only these two keys are accepted; credentials anywhere are refused. Same
 * snapshot/scratch rules as save_peer_schema; no plan or execution authority.
 * Constants validate one value, never an entire variable/mapped schema.
 */
int br_caps_save_peer_operation(const br_json *snapshot,const void *,size_t,br_json *scratch);
/* PROVISIONAL(P-20261112-1055-B), save-only {app,op,params?,schema?} / peer
 * {op,params?,schema?}. params holds concrete constants, schema final mapped
 * caps declarations from A's sole preparation chain. Each defaults to {}.
 * Disjoint whole parameter names must cover all required target parameters;
 * constants use original value validation, mapped declarations same-type
 * subset proof. No per-row/default/mapping parser or plan/runtime authority.
 * All original save snapshot/scratch/8192B/privacy rules remain. The local
 * schema key is never an OPEN field and does not enter the phone protocol. */
int br_caps_save_bindings(const br_json *snapshot,const void *,size_t,br_json *scratch);
/* Save-only required metadata for an EXISTING exact app/op/parameter mapping.
 * Revalidates the original snapshot; optional absent/false means required.
 * Unknown names or invalid/unsupported snapshots refuse. On success writes
 * only 0/1; output must be disjoint from the snapshot and input strings, and
 * stays unchanged on failure. No plan, runtime authority, allocation or IO.
 * A unions this with its script requirement before its original whole-plan
 * validation. This does not validate values/schema or supply missing mappings.
 * Current live caps still admit each eventual operation independently. */
int br_caps_save_required(const br_json *snapshot,const char *app,const char *op,
                          const char *parameter,int *required);
/* Save-only scalar type metadata for the same exact app/op/parameter.
 * The mask is the complete set of admitted JSON primitive kinds, not a value
 * validator or conversion rule. A scalar declaration yields one bit; an enum
 * unions ALL its original string/int kinds. A mixed enum is not a unique type:
 * callers without explicit source type must refuse a multi-bit result, never
 * select its first value. Existing typed sources must belong to the mask and
 * still pass original whole-value/schema and eventual current live validation.
 * Arrays and unknown mappings refuse. The complete snapshot is revalidated;
 * all required-metadata alias/unchanged-output rules apply. No IO, mutation,
 * allocation, execution authority, schema change or new wire/version field. */
enum br_caps_param_type { BR_CAPS_PARAM_STRING=1u, BR_CAPS_PARAM_INT=2u,
                          BR_CAPS_PARAM_BOOL=4u };
int br_caps_save_type_mask(const br_json *snapshot,const char *app,const char *op,
                          const char *parameter,unsigned *types);
int br_caps_save_peer_bindings(const br_json *snapshot,const void *,size_t,br_json *scratch);
/* Resolved peer request {op,params?}; validates only caps.peer_ops, executes
 * nothing. system.op's missing envelope/path policy is a separate proposal.
 */
int br_caps_peer_operation(const br_caps *,uint64_t,const void *,size_t,br_json *scratch);
/* Local-only OPEN-shaped system.op, target=system, params={op,params?}.
 * Peer declaration is admission only; caller MUST also apply A's typed local
 * operation/path allowlist. No reverse Wire OPEN or execution authority from
 * a saved snapshot. PROVISIONAL(P-20261111-1245-B). scratch wiped on return. */
int br_caps_system_request(const br_caps *,uint64_t,const void *,size_t,br_json *scratch);
int br_caps_has_feature(const br_caps *,uint64_t,const char *feature);
/* Validate the params object {app?} for an independently sent hook.describe
 * OPEN. Missing app means all. Feature + requested app must be current.
 * Returns no operation authority; descriptions remain display-only.
 */
int br_caps_describe_request(const br_caps *,uint64_t,const void *,size_t,br_json *scratch);
/* Fixed existing wire error names; never echo input/payload into diagnostics. */
const char *br_caps_error(int rc);
#endif
