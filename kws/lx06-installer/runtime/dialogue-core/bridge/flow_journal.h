/* Local API19, PROVISIONAL(P-20261112-1505-B), inherits API18 proposals.
 * No automatic replay/continuation.
 * One global serialized journal. A owns immutable binding bytes/digests and
 * authorized directory; use br_store_open_flow, hold lock through lifetime.
 * Allocate off realtime stack. No aliasing/reentry or mutation by consumers. */
#ifndef LX06_BR_FLOW_JOURNAL_H
#define LX06_BR_FLOW_JOURNAL_H
#include "flow.h"
#include "inbox_store.h"
#include "schedule.h"
#include "result_inbox.h"
#define BR_FLOW_LOG_BYTES 40960u
#define BR_FLOW_LOG_RETIRED 5u
#define BR_FLOW_LOG_BOUND 6u
#define BR_FLOW_LOG_UNSENT 7u
#define BR_FLOW_LOG_ERROR_RETIRED 8u
typedef struct {
    char task[129],app_version[129],hook_version[129],code[40];
    uint64_t received_ms,expires_ms;
} br_flow_error_witness;
typedef struct {
    char script[129],revision[65],definition_sha256[65],inputs_sha256[65],task[33];
    uint64_t serial,run;
    unsigned count,silent,audible;
} br_flow_log_binding;
int br_flow_log_binding_valid(const br_flow_log_binding *); /* read-only, no IO */
typedef struct {
    uint64_t ticket;
    unsigned local;
    br_flow_record record;
} br_flow_log_step;
typedef struct {
    uint64_t epoch,received_ms,expires_ms;
    char command[37];
} br_flow_receipt_witness;
typedef struct {
    uint64_t epoch;
    char command[37];
    unsigned ttl_s;
} br_flow_no_replay;
typedef struct {
    br_store *store; br_json *scratch;
    unsigned char *workspace; size_t capacity;
    br_flow_log_binding binding;
    unsigned phase,index,used;
    uint64_t last_ticket;
    br_flow_log_step steps[BR_FLOW_STEPS];
    unsigned char arena[BR_FLOW_ARENA];
    br_flow_receipt_witness receipt; /* schema5 only; epoch=0 means NO receipt proof */
    br_flow_no_replay no_replay; /* schema6 success/schema8 error: NEVER receipt */
    br_flow_error_witness failure; /* schema7 pair/schema8 source metadata */
    int blocked,readonly,busy;
    unsigned schema; /* Original codec version, RAM only; never a new disk field. */
} br_flow_journal;
/* Fresh storage. 0 absent/writeable, 1 valid/recovery READ ONLY,
 * 2 successful/UNSENT/error retired READ ONLY, -1 blocked.
 * Existing binding must equal every declared field; no empty fallback/reset.
 * Workspace >=40KiB, own JSON scratch; borrowed for lifetime, erased on IO.
 * Readback phase PREPARE/SUCCESS unresolved; REAPED only historical release.
 * NOT_SENT proves only previous adapter reported no side effects. */
int br_flow_journal_open(br_flow_journal *,br_store *,const br_flow_log_binding *,
                         void *workspace,size_t,br_json *scratch);
/* Restart-only exact readback of the whole original schema1..8 image. Existing
 * store lock/workspace/scratch remain borrowed and disjoint. No callbacks,
 * allocation, writes, decode replacement, receipt or release authority.
 * 1 identical, INVALID live/busy/arguments (no erase), IO absent/changed/unsafe
 * and blocks until genuine reopen. Acquired IO always erases work/scratch.
 * No continuation or replay, even on success. Serial non-realtime IO only. */
int br_flow_journal_verify(br_flow_journal *);
/* Explicit LIVE final-success counterpart: schema1/5, last REAPED only,
 * !readonly, no no-replay. Same exact original codec/workspace/store rules,
 * return codes, erasure and IO blocking as verify; readonly/busy/other phase
 * refuses BEFORE IO/erase. No K6 admission, finish or runtime authority.
 * Sole serial owner; never use this to revive a blocked or recovered flow. */
int br_flow_journal_verify_live(br_flow_journal *);
/* LIVE pending counterpart: schema3 empty BOUND or schema1 checkpoint 1..4.
 * Same original exact readback/erasure/blocking rules; no deadline proof,
 * result admission, recovery or continuation permission. Readonly/busy and
 * other schema/phase refuse without IO/erase. Serial owner as above. */
int br_flow_journal_verify_pending(br_flow_journal *);
/* After original schedule INTENT is durable, before any step. Only absent or
 * successful/UNSENT/error retired journal can become a new BOUND. API18 first
 * verifies the exact original readonly retired image after existing admission
 * gates, before copying the next binding or writing. Missing/changed bytes block
 * this handle without overwriting disk; restoring bytes cannot revive it.
 * Initial phase0 is unchanged; retirement writes have their own checks.
 * This is no source, K6, owner, release or replay authority. Recovered BOUND cannot
 * send. Atomic write failure blocks; directory-fsync ambiguity requires reopen
 * with explicit old/new binding, never a missing-file fallback. */
int br_flow_journal_bind(br_flow_journal *,const br_flow_log_binding *,const br_schedule *,unsigned slot);
/* Direct br_flow_set_checkpoint callback. Exactly 1 durable, -1 stops run.
 * API17: every existing predecessor first verifies the exact original pending
 * disk image, including all identical retries, before candidate mutation or
 * overwrite. Legacy unbound first PREPARE retains its original absent-store
 * path; scheduled flows have durable BOUND. SUCCESS always verifies. Failure
 * blocks this handle; actual reopen required. Original K6 receipt may already
 * be durable; no new aggregate or permission follows from it. Other journal
 * writes retain their own validation; this is only the checkpoint callback.
 * No logging of OPEN/credentials. Keeps prior successful result references.
 * Existing recovered journal cannot be used to send or overwritten. */
int br_flow_journal_checkpoint(void *,const br_flow *,unsigned event);
/* API19: all actual receipt/disposition/retirement writes below re-read the
 * exact original schema1..8 image AFTER original K6/owner callbacks and BEFORE
 * encoding, while retaining busy. Live and recovered predecessors use the
 * same original codec/store/workspace/scratch. Missing/changed bytes block
 * the handle without overwriting disk; restoring bytes cannot revive it.
 * Existing argument/proof rejection and idempotent NO-WRITE paths retain
 * their prior checks. Not additional authority for reads, replay or release.
 * Write readback failure: receipt/resolve/error-no-replay APIs return BR_IO;
 * retire/retire_unsent/retire_error return -1, with original fsync rules. */
/* All-success final REAPED only. Original durable schedule TERMINAL + released
 * owner + same K6 origin1 successful receipt, durable schema5 receipt witness,
 * or explicit schema6 completion-only disposition
 * required. A witness still rechecks CURRENT association/TTL even after K6
 * eviction; no new admission or sound. No deletion/rebind/send.
 * 1 durable/idempotent, -1 missing proof or IO (IO blocks handle). Recovered
 * read-only journal may retire; retired tombstone needs no renewed receipt.
 * Non-reentrant, non-realtime; original schedule/inbox immutable during call. */
/* A's current owner registry proves ALL original run/serial resources released;
 * restored owned=0 or historical journal REAPED alone is insufficient. No IO
 * side effects, reentry or mutation; exactly 1 proves release, else defer. */
typedef int (*br_flow_retire_check)(void *,const br_flow_log_binding *,uint64_t epoch,const char *command);
int br_flow_journal_retire(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *,br_flow_retire_check,void *);
/* Explicit no-dispatch retirement, NOT a successful/failed terminal receipt.
 * Empty BOUND + original durable PENDING retry + current precise owner release
 * only; recovered BOUND allowed, UNKNOWN/NOT_SENT/PREPARE never inferred safe.
 * 1 durable/idempotent UNSENT tombstone, -1 absent proof or IO (IO blocks).
 * No K6 admission, send, deletion or highwater advance. */
int br_flow_journal_retire_unsent(br_flow_journal *,const br_schedule *,unsigned slot,br_flow_retire_check,void *);
/* Explicit phone PREPARE failure retirement only. Original schedule TERMINAL,
 * exact silent origin0 step Ledger error AND scheduled aggregate origin1 error
 * (same code/text), CURRENT associations and strict release of BOTH identities.
 * No creation of receipts, schedule finish, sound, replay or continuation.
 * 1 durable/idempotent ERROR_RETIRED, -1 absent proof/IO (IO blocks handle).
 * Tombstone preserves references and historical proof, never successful output. */
int br_flow_journal_retire_error(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *,
    const br_result *remote_identity,br_flow_retire_check,void *);
/* Read-only original phone PREPARE failure. Rechecks binding, references,
 * schedule INTENT/UNKNOWN/TERMINAL and original CURRENT K6 association. Only
 * a silent origin0 error qualifies. 1 borrowed result, 0 absent, INVALID/IO;
 * out=NULL on failure. No finish, receipt creation, TTL or retirement. */
int br_flow_journal_error_result(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *,
    const br_result *remote_identity,const br_result **out);
/* Read-only original error PLUS already durable scheduled aggregate. Same
 * pair validation as retire_error, without TERMINAL/release requirement.
 * 1 exact pair, 0 missing row, INVALID/IO; no admission/TTL/playback changes.
 * CURRENT association callbacks are non-reentrant, borrowed state immutable. */
int br_flow_journal_error_receipt_check(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity);
/* Explicit restart-only error completion WITHOUT new aggregate or sound.
 * Recovered readonly phone PREPARE, original UNKNOWN/TERMINAL, exact silent
 * origin0 error STILL present and CURRENT; origin1 must be absent + authorized.
 * Resolve requires strict CURRENT release of BOTH identities, saves schema8
 * disposition BEFORE schedule finish. No text/receipt witness is fabricated.
 * Check cannot create: 1 valid disposition, 0 missing source, INVALID/IO.
 * Resolve: ACCEPTED/DUPLICATE/DEFER_LEDGER/INVALID/IO. Source metadata/times,
 * schedule identity/TTL are frozen. Source eviction defers even after save.
 * Independent retire requires TERMINAL/owned=0 + dual release; 1 durable or
 * historical duplicate, INVALID/IO otherwise. No replay/TTL/K6 mutation.
 * Borrowed state immutable, serialized non-realtime; callbacks never reenter. */
int br_flow_journal_resolve_error_no_replay(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity,br_flow_retire_check,void *);
int br_flow_journal_error_no_replay_check(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity);
int br_flow_journal_retire_error_no_replay(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity,br_flow_retire_check,void *);
/* Explicit completion of ALREADY durable readonly schema8, even after source
 * eviction. Never creates a disposition, result or text/receipt proof. Exact
 * saved source identity/version, original records, schedule/TTL and CURRENT
 * dual associations required; any existing source must match frozen metadata,
 * any origin1 conflicts. Check=1/INVALID/IO. Independent retire requires
 * TERMINAL/owned=0 + strict CURRENT dual release, writes SAME schema8 tombstone.
 * Default check/retire above still require source present. No clock/replay. */
int br_flow_journal_saved_error_no_replay_check(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity);
int br_flow_journal_retire_saved_error_no_replay(br_flow_journal *,const br_schedule *,unsigned slot,
    br_inbox *,const br_result *remote_identity,br_flow_retire_check,void *);
/* Final REAPED + actual durable scheduled origin1 success only. Capture the
 * original epoch/UUID/received/expires, NEVER a new receipt or playback state.
 * 1 durable, 2 identical witness, 4 absent receipt, -1 invalid, -2 IO (blocks).
 * Recovered readonly final journal may capture; caller serializes all state. */
int br_flow_journal_capture_receipt(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *);
/* 1 durable witness + CURRENT association, 0 no witness, -1 invalid, -2 IO.
 * Existing K6 row must agree; absent K6 is allowed ONLY with witness. No queue
 * changes/clock/TTL refresh. Witness is NOT permission to recreate a K6 row. */
int br_flow_journal_receipt_check(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *);
/* Explicit restart-only completion policy. Validates immutable original output,
 * CURRENT association + exact owner release and absence of K6 origin1 before
 * durably recording NO NEW admission. No K6 mutation, timestamps or sound.
 * 1 saved, 2 identical disposition, -1 invalid, -2 IO. Existing receipt/witness
 * rejects; default receipt recovery NEVER creates this disposition. */
int br_flow_journal_resolve_no_replay(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *,
    const br_flow_log_binding *expected,const br_flow_definition *,const br_flow_binding *,size_t,
    br_flow_retire_check,void *);
/* Actual receipt witness OR explicit completion-only disposition; 1 valid,
 * 0 absent, -1 invalid, -2 IO. These remain distinct persistent proofs. */
int br_flow_journal_completion_check(br_flow_journal *,const br_schedule *,unsigned slot,br_inbox *);
/* Readback final REAPED only, original expected binding/typed definition and
 * inputs must be immutable and correspond to A's original digests. Rechecks
 * records, declarations, private results and scheduled closed-audio/local kind.
 * 1 JSON scalar (no NUL), 0 absent default text, -1 rejected/overflow. Disjoint
 * output/length; unchanged on 0/-1. JSON scratch erased. No heap/state changes,
 * send, receipt creation, TTL reset, owner release, retirement or continuation.
 * Missing K6 row is NOT proof it was never received: do not requeue it. */
int br_flow_journal_output(br_flow_journal *,const br_flow_log_binding *expected,
                             const br_flow_definition *,const br_flow_binding *inputs,size_t input_count,
                             void *out,size_t capacity,size_t *length);
#endif
