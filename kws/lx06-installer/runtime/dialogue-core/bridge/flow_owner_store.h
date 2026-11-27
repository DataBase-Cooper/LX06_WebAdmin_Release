/* Local API15, PROVISIONAL(P-20261113-0010-B), inherits API14 proposals. Original owner storage adapter,
 * not an independent registry, Ledger, authentication or resource receipt.
 * Serialized non-realtime IO. No production wiring before A/C signoffs. */
#ifndef LX06_BR_FLOW_OWNER_STORE_H
#define LX06_BR_FLOW_OWNER_STORE_H
#include "flow_owner_handoff.h"
#define BR_FLOW_OWNER_STORE_API 15u
typedef struct {
    br_store *store;unsigned char *workspace;
    br_flow_owner_handoff_current current;void *context;
    int blocked,busy;
    br_flow_session_log *log;
    br_flow_owner_handoff_current previous;void *previous_context;
} br_flow_owner_store;
/* API10 public mutation admission precedes wire acquisition: requested AND
 * original bound log/adapter/session must be idle; live target must be the
 * original bind/hook/wire, readonly has no adapter/live store binding.
 * Required argument/relationship refusal does not erase wire or mutate state.
 * API11 bind holds original session busy during previous/current proofs and
 * owner image persistence; log install holds its own session guard. Internal
 * PREPARE extension runs inside original log/adapter/session checkpoint guards.
 * Caller still owns the full serial wire lease; no async IO may retain it. */
/* API13 same-live-session turnover checks log API14 authority after the
 * nested previous-image proof and after current-image proof before writing
 * the new owner file. Revocation blocks the original log; BR_IO requires
 * reopen/readback of BOTH files. A committed new BOUND is never rolled back.
 * No new layout/format/identity; closed/recovered operations stay offline. */
/* API14 initial bind and PREPARE also check log API15 original live authority
 * after previous/current proofs, even on refusal, before any next callback or
 * owner write. A durable PREPARE may precede loss; preserve that exact prefix
 * and block rather than repair, send, or install a legacy fallback. */
/* API15 CLOSED -> NEW session coordination uses log API17 TARGET authority
 * between previous/current image callbacks and after current before writing.
 * Old readonly/closed identity remains available for offline result/release
 * proofs; NEW binding/image publication always requires its target session.
 * No layout/format changes, rollback, automatic recovery or new send. */
/* Existing healthy br_store_open_owner, lock held for complete lifetime.
 * EXACT7388B borrowed workspace, normally original serial wire buffer prefix;
 * owner proves no pending send/async user or overlapping operation. Disjoint
 * from ALL arguments, states, results and original1536B log read workspace.
 * No allocation/IO; 1 initialized, BR_INVALID. Never reinit a live handle. */
int br_flow_owner_store_init(br_flow_owner_store *,br_store *,void *,size_t,
    br_flow_owner_handoff_current,void *);
/* API4 PROVISIONAL(P-20261112-0525-B). Explicit original BOUND-log binding.
 * INIT absent/empty image durable, then install original prepare extension.
 * Each original PREPARE durable -> exact one UNKNOWN append durable -> send.
 * Previous/current strict1 prove original locked source, not authentication.
 * Valid binding attempt failure blocks log; checkpoint failure blocks both.
 * No retry/rebind/repair/replay. Readonly recovery uses audit, never bind.
 * Same serial original wire loan and all object lifetimes; proofs no IO or
 * mutation/reentry. 1 bound, BR_INVALID/BR_IO refused. No extra image/heap. */
int br_flow_owner_store_bind(br_flow_owner_store *,br_flow_session_log *,
    br_flow_owner_handoff_current,void *);
/* Explicit STANDALONE current-owner publish only: save/publish both refuse a
 * handle with a bound log BEFORE IO/proofs/erase, even if the log is idle.
 * Use coordinated associate/retire/rebind after bind; legacy calls cannot
 * replace the identity image behind the installed two-file checkpoint.
 * previous proves exact OLD disk bytes
 * may be replaced on THIS attempt (NULL/0 = genuinely absent), strictly1.
 * Then current proves exact NEW encoded bytes belong to current original
 * owner. Both mandatory synchronous readonly proofs: no IO/reentry/mutation.
 * Missing/refused proof never writes. Corruption/IO blocks; no reset/fallback.
 * 1 durable, BR_INVALID refused, BR_IO blocked/IO. Always erase workspace on
 * acquired operation; write failure requires real reopen, never blind retry. */
int br_flow_owner_store_save(br_flow_owner_store *,const br_flow_session_log_binding *,
    const br_flow_session_log_step *,const br_result *const *,unsigned,
    br_flow_owner_handoff_current previous,void *);
/* Same serialized load/proofs/encode/durable write as save, additionally checks
 * exact INIT/PREPARE/ASSOCIATE transition against actual old disk BEFORE reuse.
 * current proves original durable PREPARE/authenticated TASK provenance, never
 * just type validity. No send barrier or cross-file transaction is installed.
 * No automatic fallback to save or retry after IO/UNKNOWN. Same return, busy,
 * erase and reopen rules. Repeated association is refused; real reopen/result
 * reconciles persisted original identity without redoing any phone action. */
int br_flow_owner_store_publish(br_flow_owner_store *,unsigned,
    const br_flow_session_log_binding *,const br_flow_session_log_step *,
    const br_result *const *,unsigned,br_flow_owner_handoff_current,void *);
/* Coordinated original authenticated TASK identity publication. Re-read exact
 * original log inside log/adapter/session guards, then original owner file;
 * complete binding/ALL prepared steps/count must match, previous strict1,
 * exactly one UNKNOWN -> known transition, current strict1, durable write.
 * Live requires this store's original bind/hook/wire; readonly genuinely
 * reopened/no adapter or live store binding. identities has count1..16 rows;
 * original identity-only br_result or NULL, no inferred authentication.
 * No IO/proof retries, fallback save, repair/replay, continuation/K6 changes.
 * Returns1/BR_INVALID/BR_IO; same busy/erase/reopen/provenance lifetime rules.
 * Callbacks cannot IO/mutate/reenter. No new state or image allocation. */
int br_flow_owner_store_associate(br_flow_owner_store *,br_flow_session_log *,
    const br_result *const *,unsigned,br_flow_owner_handoff_current,void *);
/* Closed interactive retirement only: exact original log/current owner, then
 * reload whole identity image/full binding/ALL steps/equal counts/current
 * strict1, then independent CURRENT full closed proof strictly1, then original
 * phase6 log durable. Live REAPED/closed7 requires original bind/hook/wire;
 * readonly7 requires genuinely reopened/no adapter and no live store binding.
 * Owner image/known and unknown TASKs retained byte-exact. No K6/Ledger IO,
 * deletion, release, repair, continuation, replay or new-intent permission.
 * Same serial wire lease/guards/erase rules. Closed callback no IO/mutation/
 * reentry. 1 log durable, BR_INVALID refusal, BR_IO either file IO/blocked;
 * ambiguity requires real reopen. No new state/image/schema allocation. */
int br_flow_owner_store_retire_closed(br_flow_owner_store *,br_flow_session_log *,
    br_flow_session_log_closed,void *);
/* Explicit NEW intent from exact phase6; current whole old image/previous and
 * independent complete closed proof strictly1, then new log BOUND durable,
 * new empty owner image/current strict1 durable, original PREPARE hook.
 * Four identities increase; new original adapter/wire; no same WSS/scheduled.
 * Live requires this store's original hook; readonly genuinely reopened with
 * no adapter/live store binding. Current must prove BOTH old/new provenance.
 * Old image retained until current closed and new image provenance accepted.
 * No K6/Ledger writes, release, replay, repair or proof/source synthesis.
 * Failure after log replacement blocks checkpoint; IO needs true reopen,
 * cross-binding conflict remains UNKNOWN, aligned BOUND still readonly.
 * Same guards/serial lease/erase rules, callbacks no IO/mutation/reentry.
 * contexts may be NULL; previous/context retained for later PREPAREs.
 * 1 durable; BR_INVALID refusal/proof; BR_IO either file IO/blocked. */
int br_flow_owner_store_rebind_closed(br_flow_owner_store *,br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,br_flow_owner_handoff_current,void *,
    br_flow_session_log_closed,void *);
/* API8 PROVISIONAL(P-20261112-0650-B). LIVE interactive cycle only, original
 * store/log/prepare hook/wire. Full old identity/current provenance and
 * CURRENT full old-cycle handoff proof, then phase7 log durable; owner image
 * retained byte-exact. No session close or old-obligation release action. */
int br_flow_owner_store_retire_live(br_flow_owner_store *,br_flow_session_log *,
    br_flow_session_log_handed_off,void *);
/* Phase7 LIVE only, SAME original adapter/session/epoch/script/revision and
 * full original tickets retained, entire new FLOW must fit capacity32.
 * Old disk/owner, whole image/previous/current/full prefix and full CURRENT
 * old-cycle handoff strictly1 -> new owner -> BOUND durable -> new empty image
 * current/durable -> original PREPARE barrier. No readonly or scheduled path.
 * Failure semantics/guards/serial wire lease/contexts match rebind_closed;
 * no new state/image/schema/heap, reconnect, release, repair or replay. */
int br_flow_owner_store_rebind_live(br_flow_owner_store *,br_flow_session_log *,
    const br_flow_session_log_binding *,br_flow_session *,br_flow_owner_handoff_current,void *,
    br_flow_session_log_handed_off,void *);
/* API9: result/audit require readonly log/no adapter before borrowing wire.
 * Target OR original bound log/adapter/session busy refuses before any IO or
 * workspace erase, preserving the outer original frame. out is disjoint.
 * Re-load EVERY query, whole-image validate, original readonly log/current
 * provenance + independent K6 check, then erase workspace AFTER K6 returns.
 * No cached image/proof. 1 borrowed original K6 result, 0 K6 absent/unresolved,
 * BR_INVALID missing/unknown/refused owner association, BR_IO corrupt/blocked.
 * Failure out=NULL. No writes/playback/continuation/retirement/receipt creation.
 * API12 holds original bound log/adapter/session busy through ALL callbacks
 * and workspace erase. Audit uses the original readonly inspect visitor so
 * its target stays guarded through image/current checks. Busy admission does
 * not acquire/clear somebody else's guard; no original binding is also valid.
 * Callbacks retain the original no-IO/no-reentry rules. Serial wire lease must
 * cover this whole call, including K6 callbacks. Same busy/erase rules. */
int br_flow_owner_store_result(br_flow_owner_store *,br_flow_session_log *,unsigned,
    br_inbox *,const br_result *,const br_result **);
enum br_flow_owner_audit {
    BR_OWNER_ALIGNED=1,BR_OWNER_LOG_AHEAD,BR_OWNER_IMAGE_AHEAD,BR_OWNER_MISSING
};
/* Reopened readonly log/no adapter only; rereads BOTH locked files per call.
 * Recovery checks exact log/current owner, then whole owner image/current
 * strict1 and common prefix. ALIGNED equal counts (unknown TASKs permitted);
 * LOG_AHEAD exactly one durable PREPARE missing from image; IMAGE_AHEAD exactly
 * one image row more than log; MISSING absent image (no provenance callback).
 * All are diagnostic only: NEVER send/retry/continue/repair/retire or infer
 * completion/release. 2/3/4 remain UNKNOWN with original obligations retained.
 * BR_INVALID conflict/missing proof/live/reentry; BR_IO log readback refusal
 * or owner corruption/IO. Same serial lease/busy/erase rules as result; no
 * writes, new state, allocations or cross-file transaction. No cached proof.
 * Same API12 full original-wire guards as result; no target/original alias. */
int br_flow_owner_store_audit(br_flow_owner_store *,br_flow_session_log *);
#endif
