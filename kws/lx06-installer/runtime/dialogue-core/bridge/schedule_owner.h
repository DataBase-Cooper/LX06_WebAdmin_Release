/* Local API2, PROVISIONAL(P-20261112-1635-B), inherits P-1610. Original private owner file,
 * scheduled identity envelope only. No production attachment or send barrier.
 * Serial non-realtime IO; all borrowed arguments immutable/disjoint. */
#ifndef LX06_BR_SCHEDULE_OWNER_H
#define LX06_BR_SCHEDULE_OWNER_H
#include "schedule_session.h"
#include "flow_owner_store.h"
#define BR_SCHEDULE_OWNER_API 3u
#define BR_SCHEDULE_OWNER_HEADER 92u
#define BR_SCHEDULE_OWNER_BYTES (BR_SCHEDULE_OWNER_HEADER+BR_FLOW_OWNER_HANDOFF_BYTES)
/* Canonical scheduled envelope + complete ORIGINAL owner codec, at most16
 * prepared rows. Original per-step identities/unknowns are preserved exactly.
 * No results, timestamps, tokens or release/receipt claims. EXACT7480B output
 * capacity, length=0 on failure; acquired output erased on failure. */
int br_schedule_owner_encode(const br_flow_log_binding *,const br_flow_session_log_binding *,
    const char command[37],const br_flow_session_log_step *,const br_result *const *,unsigned,
    void *,size_t,size_t *);
/* Whole envelope/inner image and all supplied rows validated on EVERY call.
 * 1 identity match, BR_INVALID refusal; published cleared on failure. Prefix
 * counts may differ for diagnostic use only, never replay/repair permission. */
int br_schedule_owner_prefix(const void *,size_t,const br_flow_log_binding *,
    const br_flow_session_log_binding *,const char command[37],
    const br_flow_session_log_step *,unsigned,unsigned *published);
/* Pure transition check; original INIT/PREPARE/ASSOCIATE semantics, now also
 * require the exact scheduled envelope. INIT=NULL/0 old image/count0 only.
 * Validate the WHOLE image and all typed rows, no prefix-only authority.
 * 1 valid transition, BR_INVALID refusal; no IO/proofs or durable authority. */
int br_schedule_owner_transition(const void *,size_t,unsigned,
    const br_flow_log_binding *,const br_flow_session_log_binding *,const char command[37],
    const br_flow_session_log_step *,const br_result *const *,unsigned);
/* In-place PREPARE or ASSOCIATE only (never INIT). EXACT7480B capacity;
 * length=old size in/new size out. Full transition validated BEFORE mutation;
 * refusal leaves whole buffer/length unchanged. Preserve exact92B envelope
 * and all prior associations, reuse original append/encoder, no extra image.
 * Borrowed inputs disjoint/immutable. Pure codec, no writes/authentication,
 * K6/TTL, send/continuation/recovery/retirement or installed log barrier. */
int br_schedule_owner_apply(void *,size_t,size_t *,unsigned,
    const br_flow_log_binding *,const br_flow_session_log_binding *,const char command[37],
    const br_flow_session_log_step *,const br_result *const *,unsigned);
/* Original br_flow_owner_store_init handle, original br_store_open_owner lock,
 * no bound log. Loan is the SAME original serial wire prefix, now EXACT7480B
 * accessible capacity (within8240B wire); no additional image/handle allocation.
 * Entire wire lease covers this call/callbacks/erase; no pending async user.
 * Seed: original first scheduled INTENT/begin boundary, ABSENT owner file only.
 * previous(NULL,0) + current(exact NEW bytes), both strictly1, prove current
 * locked original provenance; no IO/mutation/reentry in either callback.
 * Recheck absence after callbacks, durable identity only. No session attach,
 * prepared hook or continuation. A failed write blocks until genuine reopen.
 * 1 durable, BR_INVALID refusal/existing file, BR_IO corrupt/unsafe/ambiguous.
 * Legacy owner images are never migrated, rebound or overwritten. */
int br_schedule_owner_seed(br_flow_owner_store *,size_t,const br_schedule *,unsigned,
    const br_flow *,const br_flow_log_binding *,const br_flow_session_log_binding *,
    const char command[37],br_flow_owner_handoff_current previous,void *);
/* Read ONLY, including after genuine process restart. Reload the original
 * locked owner file per query, check whole identity/prefix + current strict1,
 * erase wire before returning. 1 match, 0 absent (unresolved), BR_INVALID
 * identity/proof/admission refusal, BR_IO invalid/unsafe store (blocks handle).
 * No cached authority, K6 access, TTL change, result/playback, write/repair,
 * continuation or retirement. log must remain NULL. published=0 on failure. */
int br_schedule_owner_inspect(br_flow_owner_store *,size_t,const br_flow_log_binding *,
    const br_flow_session_log_binding *,const char command[37],
    const br_flow_session_log_step *,unsigned,unsigned *published);
/* Original log's designated PREPARE extension ONLY, under its already held
 * log/adapter/session guards. Whole old scheduled image/current provenance,
 * exactly one UNKNOWN append, new provenance/current caps, durable write.
 * Same original wire loan, no new codec/schema, retry, association or retire.
 * A failed/ambiguous prefix blocks BOTH handles until genuine reopen. */
int br_schedule_owner_prepare(br_flow_owner_store *,br_flow_session_log *,
    const br_flow_log_binding *,const char command[37]);
/* Explicit scheduled successor, original phase8 only. Preserve and compare
 * the ENTIRE old owner image in the original runner's otherwise idle wire.
 * Old prefix/current provenance/previous/full closed proof precede durable
 * new BOUND; then exact old image is reread before durable new owner seed.
 * Any ambiguous prefix blocks both handles, never delete/repair/replay.
 * old_image is disjoint borrowed >=BR_SCHEDULE_OWNER_BYTES, erased on return;
 * the supplied prepared hook is installed only after both durable writes. */
int br_schedule_owner_turnover(br_flow_owner_store *,br_flow_session_log *,br_flow_session *,
    const br_flow_log_binding *old,const char old_command[37],
    const br_flow_log_binding *next,const br_flow_session_log_binding *,const char next_command[37],
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *,
    br_flow_checkpoint,void *,void *old_image,size_t);
/* Same publication order for explicit live schema11/phase9 ZERO-row owner.
 * Current closed proof receives count0. Old success API remains phase8 only. */
int br_schedule_owner_retry(br_flow_owner_store *,br_flow_session_log *,br_flow_session *,
    const br_flow_log_binding *old,const char old_command[37],
    const br_flow_log_binding *next,const br_flow_session_log_binding *,const char next_command[37],
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *,
    br_flow_checkpoint,void *,void *old_image,size_t);
/* Distinct live phase11 failed-prefix successor; original failure receipt
 * and current actual closure remain mandatory coordinator proofs. */
int br_schedule_owner_after_failure(br_flow_owner_store *,br_flow_session_log *,br_flow_session *,
    const br_flow_log_binding *old,const char old_command[37],
    const br_flow_log_binding *next,const br_flow_session_log_binding *,const char next_command[37],
    br_flow_owner_handoff_current,void *,br_flow_session_log_closed,void *,
    br_flow_checkpoint,void *,void *old_image,size_t);
/* Current live TASK writer, called only under original log/adapter/session
 * inspect guards. Exact current row must originate in the original operation;
 * current() revalidates that borrowed source, scheduler and caps after every
 * callback/durable boundary. Original idle runner wire is a disjoint old-image
 * loan, erased on return. Full old owner reread precedes replacement; exact
 * new owner readback follows it. Repeated exact association makes no write.
 * Ambiguous/conflicting provenance blocks both handles, preserves obligations.
 * No result is stored here; no TASK terminalization, K6, release or replay. */
int br_schedule_owner_associate(br_flow_owner_store *,br_flow_session_log *,const br_flow_log_binding *,
    const char command[37],unsigned,const br_result *,int (*current)(void *),void *,void *,size_t);
/* Full scheduled wrapper + original pure codec match and current provenance,
 * reloaded on every call. Perform this IO outside any no-IO callback.
 * count must be the complete prepared log prefix; no repair, continuation,
 * receipt creation, TTL change or retirement. */
int br_schedule_owner_associated(br_flow_owner_store *,const br_flow_log_binding *,
    const br_flow_session_log_binding *,const char command[37],const br_flow_session_log_step *,
    unsigned count,unsigned index,const br_result *);
/* Original readonly flow-session Ledger + exact scheduled owner composition.
 * Loads/validates the full original owner BEFORE mandatory no-IO callbacks,
 * then holds its original serialized lock/wire throughout original K6 query.
 * Mandatory association remains the original pure codec and current immutable
 * provenance; K6 independently invokes its own original current association.
 * Neither callback may perform IO, mutate/reenter or create authority. Returns
 * the original borrowed K6 result only. No repair, TTL/write/playback/release. */
int br_schedule_owner_ledger_result(br_flow_owner_store *,const br_flow_log_binding *,
    br_flow_session_log *,const char command[37],unsigned,br_inbox *,const br_result *,const br_result **);

#endif
