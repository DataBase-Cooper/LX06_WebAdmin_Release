/* Local API22, PROVISIONAL(P-20261113-0029-B), inherits API21 proposals.
 * Identity-only fixed WSS log, explicit interactive success retirement.
 * Uses the original global flow store/lock. Legacy journals refuse schema9;
 * this log refuses legacy bytes. Separate schema10 holds only the explicit
 * scheduled-success tombstone; schema11 is distinct zero-row scheduled unsent.
 * Schema12 keeps the confirmed failure code and admitted partial prefix.
 * No result prose, receipts or replay. Terminal fingerprint stays RAM-only.
 * Serial, non-realtime IO. Immutable binding/config/owner state, no reentry.
 * Keep adapter, store lock and disjoint workspace alive through actual drain.
 * Allocate off realtime stack. No production wiring before A/C signoff. */
#ifndef LX06_BR_FLOW_SESSION_LOG_H
#define LX06_BR_FLOW_SESSION_LOG_H
#include "flow_session.h"
#include "inbox_store.h"
#define BR_FLOW_SESSION_LOG_API 22u
#define BR_FLOW_SESSION_LOG_BYTES 1536u
#define BR_FLOW_SESSION_LOG_BOUND 5u
#define BR_FLOW_SESSION_LOG_RETIRED 6u
#define BR_FLOW_SESSION_LOG_RETIRED_LIVE 7u
#define BR_FLOW_SESSION_LOG_SCHEDULED_RETIRED 8u
#define BR_FLOW_SESSION_LOG_SCHEDULED_UNSENT 9u
#define BR_FLOW_SESSION_LOG_SCHEDULED_FAILED 10u
#define BR_FLOW_SESSION_LOG_FAILURE_RETIRED 11u
enum br_flow_log_source { BR_LOG_OFFICIAL=1,BR_LOG_LOCAL,BR_LOG_SCHEDULED };
typedef struct {
    char script[129],flow_sha256[65],revision[65],definition_sha256[65],inputs_sha256[65];
    uint64_t run,session_run,epoch,generation,ctl_run,cycle,job;
    unsigned count,source;
} br_flow_session_log_binding;
/* Pure original binding validation, no IO/owner proof or normalization.
 * Optional handoff consumers reuse the exact schema9 identity rules. */
int br_flow_session_log_binding_valid(const br_flow_session_log_binding *);
typedef struct { uint64_t ticket;char command[37]; } br_flow_session_log_step;
/* Exactly 1 validates the ORIGINAL current owner/config binding. No mutation,
 * IO, reentry, resource release or receipt creation inside this callback. */
typedef int (*br_flow_session_log_owner)(void *,const br_flow_session_log_binding *);
typedef struct {
    br_store *store;unsigned char *workspace;br_flow_session *adapter;
    br_flow_session_log_owner owner;void *context;
    br_flow_session_log_binding binding;
    br_flow_session_log_step steps[BR_FLOW_STEPS];
    unsigned phase,index;int readonly,blocked,busy;
    char failure_code[40];
    unsigned char failure_hash[32]; /* RAM-only original session fingerprint; NEVER serialized */
    br_flow_checkpoint prepared;void *prepared_context;
} br_flow_session_log;
/* Fresh handle: 0 absent, 1 valid readonly (every binding field must match),
 * -1 blocked. Store is already br_store_open_flow. No missing-file fallback.
 * Workspace exactly1536B, disjoint, erased on IO. No schema migration. */
int br_flow_session_log_open(br_flow_session_log *,br_store *,
    const br_flow_session_log_binding *,void *,size_t,br_flow_session_log_owner,void *);
/* Absent only, current idle original fixed-session adapter. Durable BOUND
 * before installing its specialized barrier; existing file is never rebound.
 * 1 attached, -1 refusal/IO. IO ambiguity blocks until real reopen/readback. */
int br_flow_session_log_attach(br_flow_session_log *,br_flow_session *);
/* API8 PROVISIONAL(P-20261112-0525-B). Optional single PREPARE extension,
 * installed only on live attached BOUND before first dispatch. Rechecks exact
 * disk + strict1 owner. Extension runs AFTER original PREPARE is durable,
 * BEFORE returning to original start/send; strict1 required, failure blocks.
 * Old log is reread before every checkpoint when installed. Same serial,
 * synchronous/no-reentry lifetime rules; extension may do bounded private IO.
 * Legacy rebind refuses installed hooks; API11 coordinates replacement.
 * No readonly install or auto recovery.
 * 1 installed, -1 refusal/IO. No disk/schema change from installation. */
int br_flow_session_log_set_prepare(br_flow_session_log *,br_flow_checkpoint,void *);
/* API15 attach/install/PREPARE recheck original live discovery/caps authority
 * after every owner proof, including strict-proof refusal. Loss blocks before
 * the next write/hook/send. Persisted prefixes remain; no rollback/retry.
 * SUCCESS/REAPED and closed/readonly recovery retain original semantics. */
/* API16 also rechecks authority after the optional generic PREPARE extension,
 * regardless of its return value. Preserve every committed prefix and block
 * before start/send on revocation; no retry, rollback or recovery admission. */
/* API13: attach/install/checkpoint callbacks hold original log, adapter AND
 * session busy. Checkpoint preserves caller's adapter busy (normal driver
 * holds it already); missing adapter/session or outer session busy refuses
 * before IO/proof/workspace erase. All acquired guards restored on failure.
 * Serial synchronous reentry protection, not a cross-thread mutex. Proofs
 * remain readonly/no reentry; only designated extension has bounded IO. */
int br_flow_session_log_checkpoint(void *,const br_flow *,unsigned event);
/* Readonly recovery inspection only: exact disk readback on EVERY call and
 * CURRENT owner strictly1 required. Disk mismatch/missing blocks the handle.
 * Historical phase is NOT current release/terminal receipt authority.
 * Returns1 borrowed original log, -1 refusal; out=NULL on failure.
 * No continuation, K6 query/creation, TTL, playback, retirement or writes. */
int br_flow_session_log_recovery(br_flow_session_log *,const br_flow_session_log **out);
/* API9: exact disk/current-owner snapshot within the original log AND live
 * adapter/session busy guards. Readonly requires no adapter; live requires
 * original bound identities, possibly closed, no concurrent operation.
 * visitor may do bounded private owner-file IO, never mutate/reenter original
 * objects. Strict1 visitor =>1, else -1; disk mismatch blocks original log.
 * Borrowed snapshot valid ONLY in visitor. Always erase log read workspace.
 * No writes/new state/repair/replay or authentication/release authority. */
typedef int (*br_flow_session_log_visitor)(void *,const br_flow_session_log *);
int br_flow_session_log_inspect(br_flow_session_log *,br_flow_session_log_visitor,void *);
/* Interactive successful LIVE log only; not recovered REAPED or scheduled.
 * Original FLOW DONE, session closed/unowned, every original record retained.
 * released is a mandatory strict1 CURRENT registry/CTL/result-handoff proof,
 * not a release action, historical phase or K6 receipt. No IO/mutation/reentry.
 * Writes a complete identity tombstone; does not delete or clear live objects.
 * 1 durable, -1 refusal/IO; disk mismatch/IO blocks until real reopen. */
int br_flow_session_log_retire(br_flow_session_log *,br_flow_session_log_owner released,void *);
/* Isolated scheduled-success retirement, schema10/phase8 ONLY. Original
 * schema9 still rejects scheduled phase6/7; interactive APIs remain unchanged.
 * Requires live original DONE/REAPED records, closed/unowned original session,
 * exact disk and current owner. Strict1 visitor proves original durable
 * schedule TERMINAL, complete scheduled owner prefix, aggregate K6 and current
 * actual reap while log/adapter/session guards are held. No receipt creation,
 * source release, new intent, tombstone reuse or recovered continuation.
 * Failure/ambiguous write retains all ownership and requires real reopen. */
int br_flow_session_log_retire_scheduled(br_flow_session_log *,br_flow_session_log_visitor,void *);
/* Explicit first-step never-sent path only, pristine original session with
 * no admitted ticket. Exact live BOUND and mandatory owner/INTENT visitor;
 * cancel uses original FLOW NEXT preemption, never fabricates success/timeout.
 * Cancellation preserves BOUND and all holds. Close/reap is a separate owner
 * action. retire_unsent requires that same CANCELLED flow, actually closed
 * unowned session and a CURRENT resource proof in visitor, then writes only
 * distinct schema11/phase9 with ZERO step rows. Readonly/reopened handles,
 * PREPARE, UNKNOWN and any dispatched ticket always refuse. No automatic
 * retry/rebind, K6 creation or source release. IO ambiguity blocks until reopen. */
int br_flow_session_log_cancel_unsent(br_flow_session_log *,uint64_t,
    br_flow_session_log_visitor,void *);
int br_flow_session_log_retire_unsent(br_flow_session_log *,
    br_flow_session_log_visitor,void *);
/* Explicit current scheduled terminal ERROR only. Original session validated
 * its full terminal fingerprint; exact PREPARE prefix/owner before visitor.
 * Visitor persists original aggregate failure K6 + scheduler TERMINAL. Then
 * distinct schema12/phase10 preserves prefix and original code, never success.
 * RAM fingerprint is never serialized; reopening is observation only.
 * A separate phase11 retirement needs original FAILED drain, actual closed
 * unowned session, exact prefix/fingerprint/code and CURRENT visitor proof.
 * Neither entry closes, reaps, releases sources or permits continuation. */
int br_flow_session_log_record_failure(br_flow_session_log *,br_flow_session_log_visitor,void *);
int br_flow_session_log_retire_failure(br_flow_session_log *,br_flow_session_log_visitor,void *);
/* Explicit NEW intent only, RETIRED log (live or reopened) and fresh adapter.
 * Old adapter must first be cleared. All four run/session_run/epoch/generation
 * strictly increase; interactive source only. Rechecks old release + new owner.
 * Durable new BOUND before installing barrier. No same-session replacement,
 * replay, automatic begin, scheduled retirement or Ledger changes. */
int br_flow_session_log_rebind(br_flow_session_log *,const br_flow_session_log_binding *,
    br_flow_session *,br_flow_session_log_owner released,void *);
/* Explicit completed interactive FLOW on the SAME live session. Full original
 * ticket suffix + strict current release/CTL/result-handoff proof required.
 * Phase7 is distinct from closed retirement; recovered phase7 is readonly.
 * Same serial/reentry/IO rules as retire. No automatic clear or continuation. */
int br_flow_session_log_retire_live(br_flow_session_log *,br_flow_session_log_owner released,void *);
/* API14: live retirement/turnover rechecks original discovery READY, epoch,
 * caps and live/unowned session AFTER every owner/visitor/handoff callback.
 * Revocation blocks the handle; no later callback or log write is admitted.
 * Reopen/readback is observation only, never automatic rebind or replay.
 * A loss during the post-BOUND owner-image callback leaves that committed
 * prefix intact and blocks; it cannot be rolled back or reported successful.
 * Original serial owner must handle the disconnect after these guards return.
 * No layout/schema/wire change. Closed/recovered retirement remains offline. */
/* Coordinator-only nested check under already held log/adapter/session guards.
 * Strict1 current authority, -1 refusal (revocation blocks). No IO or action,
 * no stand-alone entry, release proof, new identity or runtime admission. */
int br_flow_session_log_live_current(br_flow_session_log *);
/* API17: explicit phase6 -> NEW live session handoff rechecks the TARGET
 * before IO and after every proof/visitor, regardless of strict proof value.
 * Coordinator-only, log + target adapter/session busy already held. Original
 * old binding may be readonly/closed; it is not the target connection.
 * 1 current target identities/discovery/caps, -1 refusal; loss blocks log.
 * Revoked target also installs a refusing checkpoint on a fresh FLOW, even
 * through legacy/full-closed APIs, so restored caps cannot bypass this log.
 * Existing unrelated legacy IO-refusal/checkpoint semantics remain unchanged.
 * No IO/new admission/release or repair. Persisted prefixes remain intact. */
int br_flow_session_log_target_current(br_flow_session_log *,br_flow_session *,
    const br_flow_session_log_binding *);
/* Phase7 LIVE handle only, SAME adapter/session after original clear + bind.
 * run/generation increase, session_run/epoch/script/revision stay fixed.
 * CTL owner unchanged, cycle increases, job is original per-cycle nonzero ID
 * (or all three remain zero). New owner proves the exact cycle/job pair.
 * All original session tickets retained; capacity for the ENTIRE new FLOW.
 * Recheck old release + new owner, durable BOUND before checkpoint. */
/* API18: a legacy same-session rebind that observes authority loss during a
 * proof installs the original refusing checkpoint before returning. A later
 * caps recovery cannot dispatch the fresh FLOW without its durable handoff.
 * No new layout/format/state or IO. Unrelated legacy IO refusals retain their
 * previous checkpoint behavior; this does not authorize retry or recovery. */
int br_flow_session_log_rebind_live(br_flow_session_log *,const br_flow_session_log_binding *,
    br_flow_session *,br_flow_session_log_owner released,void *);
/* LIVE phase7 handle only, AFTER original session actually closed/unowned.
 * Original session identity and full ticket suffix must remain unchanged.
 * Adapter must retain original DONE/checkpoint, or be completely cleared.
 * Mandatory CURRENT strict1 owner + closed-resource/CTL/handoff proof;
 * close request or historical phase alone is never release authority.
 * Explicitly writes phase6 with all original identities unchanged. No close,
 * clear, release, replay or automatic rebind. Recovered phase7 stays readonly.
 * Same disk/reentry/IO rules, return1 durable, -1 refusal/IO. */
int br_flow_session_log_close_retired_live(br_flow_session_log *,br_flow_session_log_owner released,void *);
/* Mandatory strict1 CURRENT original persistent registry proof for the ENTIRE
 * binding + ordered steps. Proves actual original session closure, resources,
 * CTL/producer drain and result handoff with no outstanding obligations.
 * Not a release action, historical tombstone, timeout or Ledger receipt.
 * Borrowed immutable arguments, synchronous readonly; no IO/reentry/mutation. */
typedef int (*br_flow_session_log_closed)(void *,const br_flow_session_log_binding *,
    const br_flow_session_log_step *,unsigned);
/* Explicit recovered phase7 -> phase6 only. Reopened readonly, no adapter,
 * official/local, full log. Rechecks exact disk + current owner + closed proof.
 * No synthetic live session, replay/continuation/playback/Ledger changes.
 * Success stays readonly; NEW intent must use original strict rebind API.
 * 1 durable, -1 refusal/IO. Disk/IO ambiguity blocks until real reopen.
 * Same serial/busy/workspace rules. No production without real registry. */
int br_flow_session_log_recover_closed(br_flow_session_log *,br_flow_session_log_closed,void *);
/* API10: explicit CLOSED retirement with a bounded private-file visitor BEFORE
 * mandatory CURRENT full binding/steps closed proof, both strictly1. Visitor
 * obeys inspect's bounded IO/no-mutation/no-reentry contract. Same original
 * guards/disk-owner checks as retire (live REAPED), close_retired_live (live7)
 * or recover_closed (readonly7). All paths write only original phase6 log;
 * readonly stays readonly. No identity deletion/replacement/new intent or
 * inferred release. 1 durable, -1 refused/IO; original reopen rules apply. */
int br_flow_session_log_retire_inspected(br_flow_session_log *,
    br_flow_session_log_visitor,br_flow_session_log_closed,void *);
/* Explicit NEW intent from complete phase6 (live or readonly reopened).
 * Same four-identity/new adapter/owner/durable barrier rules as rebind, with
 * mandatory CURRENT closed proof for the ENTIRE old binding + ordered steps
 * on EVERY attempt, before replacing the old log. Historical phase6 or an
 * earlier successful proof never substitutes for current original registry.
 * No cached proof, new registry, release, continuation or Ledger changes.
 * Same busy/disk/IO/workspace rules; 1 durable, -1 refusal/IO. */
int br_flow_session_log_rebind_closed(br_flow_session_log *,const br_flow_session_log_binding *,
    br_flow_session *,br_flow_session_log_closed,void *);
/* Coordinated NEW interactive intent, same four-increasing identities/phase6
 * rules. Existing prepared hook accepted only here. All callbacks mandatory.
 * Exact old log/old owner -> before -> CURRENT full closed -> new owner ->
 * new BOUND durable -> original checkpoint -> after -> new prepared hook.
 * Visitors may do bounded identity IO, not mutate/reenter objects. after sees
 * NEW BOUND; failure blocks log/checkpoint, no send/retry/automatic repair.
 * contexts borrowed synchronously except prepared_context, retained through
 * actual drain. Both files/serial wire locked throughout. No transaction,
 * authentication, release or new state/schema. 1 durable, -1 refusal/IO. */
typedef struct {
    br_flow_session_log_visitor before,after;
    br_flow_session_log_closed closed;
    br_flow_checkpoint prepared;
    void *context,*prepared_context;
} br_flow_session_log_rebind_ops;
int br_flow_session_log_rebind_inspected(br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,const br_flow_session_log_rebind_ops *);
/* Explicit scheduled successor only: old schema10/phase8, both scheduled,
 * no CTL identity, four strictly increasing run/session/epoch/generation IDs.
 * Same exact old disk/owner, full CURRENT closed proof, fresh original adapter,
 * durable BOUND then inspected owner replacement and installed PREPARE hook.
 * Existing interactive rebind APIs continue refusing scheduled identities.
 * This is ordered publication, not an atomic multi-file transaction. */
int br_flow_session_log_rebind_scheduled_inspected(br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,const br_flow_session_log_rebind_ops *);
/* Read exact retired scheduled bytes after the original adapter is cleared.
 * Full old owner/receipt visitor and CURRENT old resource closure mandatory;
 * no new intent, binding, send, retirement, or recovery authority is issued. */
int br_flow_session_log_inspect_scheduled_retired(br_flow_session_log *,
    br_flow_session_log_visitor,br_flow_session_log_closed,void *);
/* Separate live schema11/phase9 successor path. Closed proof receives ZERO
 * admitted steps, while binding.count retains configured FLOW length. Exact
 * old empty prefix and CURRENT original closure on every attempt; four new
 * identities strictly increase. Reopened handles remain observation only.
 * Old phase8/interactive APIs keep rejecting phase9. Ordered publication has
 * the same old-byte/new-owner/authority guards as scheduled success turnover. */
int br_flow_session_log_inspect_unsent(br_flow_session_log *,
    br_flow_session_log_visitor,br_flow_session_log_closed,void *);
int br_flow_session_log_rebind_unsent_inspected(br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,const br_flow_session_log_rebind_ops *);
/* API22: explicit live schema12/phase11 successor. Only the original failed
 * dispatched prefix is passed to CURRENT closure, with original failure K6
 * verified by the coordinator. Readonly reopen and existing phase8/phase9
 * APIs cannot admit this phase. Four identities strictly increase; ordered
 * old-prefix/new-BOUND/new-owner guards remain unchanged. No replay. */
int br_flow_session_log_inspect_failure_retired(br_flow_session_log *,
    br_flow_session_log_visitor,br_flow_session_log_closed,void *);
int br_flow_session_log_rebind_failure_inspected(br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,const br_flow_session_log_rebind_ops *);
/* API12 PROVISIONAL(P-20261112-0650-B). Full CURRENT old-cycle handoff proof,
 * same signature/lifetime as closed, but session MUST remain live/unowned.
 * Proves old CTL/producer drain and result handoff with no old obligations;
 * never session closure, release action, historical phase or Ledger receipt. */
typedef br_flow_session_log_closed br_flow_session_log_handed_off;
int br_flow_session_log_retire_live_inspected(br_flow_session_log *,
    br_flow_session_log_visitor,br_flow_session_log_handed_off,void *);
typedef struct {
    br_flow_session_log_visitor before,after;
    br_flow_session_log_handed_off handed_off;
    br_flow_checkpoint prepared;
    void *context,*prepared_context;
} br_flow_session_log_turnover_ops;
/* Same original live rebind checks, SAME adapter/session/epoch and complete
 * original ticket suffix retained; capacity for ENTIRE new FLOW required.
 * Accepts installed prepare hook only through this explicit coordination.
 * Old disk/owner -> before -> full handoff -> new owner -> BOUND durable ->
 * checkpoint -> after durable -> prepared hook. Failure after log mutation
 * blocks checkpoint; readonly recovery cannot continue/repair/replay.
 * Same guards/borrowed contexts/no state or schema allocation as API11. */
int br_flow_session_log_rebind_live_inspected(br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,const br_flow_session_log_turnover_ops *);
#endif
