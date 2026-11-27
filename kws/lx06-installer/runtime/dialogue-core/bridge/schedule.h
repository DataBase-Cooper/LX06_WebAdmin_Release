/* Isolated scheduler core API 3, PROVISIONAL(P-20261111-1502-B).
 * No timer thread, settings parser, transport, phone Ledger or playback.
 * One serialized instance per script. Borrow immutable config until close.
 * Place state off realtime stack. IO callbacks must not reenter this API. */
#ifndef LX06_BR_SCHEDULE_H
#define LX06_BR_SCHEDULE_H
#include "schedule_calendar.h"
#include <stddef.h>
struct br_schedule_guard;
#define BR_SCHEDULE_TASKS 32u
#define BR_SCHEDULE_HISTORY 64u
#define BR_SCHEDULE_BYTES 16384u
enum br_schedule_phase { BR_SCHED_WAITING=0,BR_SCHED_PENDING,BR_SCHED_INTENT,
    BR_SCHED_RUNNING,BR_SCHED_UNKNOWN,BR_SCHED_TERMINAL };
typedef struct {
    char id[33]; br_calendar calendar;
    unsigned enabled,audible,grace_s,announce_ttl_s;
    uint64_t seed; /* owner entropy for NEW state; loaded state owns saved seed */
} br_schedule_task;
typedef struct {
    void *context;
    /* read: 0 absent/1 bytes/-1 unsafe; write: 0 durable/-1 failure.
     * Use br_store_open_schedule on A's authorized private directory FD. */
    int (*read)(void *,void *,size_t,size_t *);
    int (*write)(void *,const void *,size_t);
} br_schedule_io;
typedef struct {
    enum br_schedule_phase phase;
    int32_t day,highwater;
    int64_t due;
    uint64_t seed,serial,epoch,skipped; /* skipped = skipped ranges, not days */
    char command[37],remote[129];
    unsigned attempts,auth_blocked;
    uint64_t retry_since,retry_delay; /* monotonic, RAM only */
} br_schedule_record;
/* Quiescent removed IDs only. No result receipt or unresolved action lives here.
 * Retained until the same ID is restored; never evicted to admit a new ID. */
typedef struct {
    char id[33]; int32_t highwater;
    uint64_t seed,skipped;
    unsigned auth_blocked;
} br_schedule_history;
/* Zero initialize once. Shared by serial instances; busy is NOT a thread lock.
 * All read/write paths erase bytes, including corrupt/IO failure paths. */
typedef struct {
    unsigned char bytes[BR_SCHEDULE_BYTES];
    int busy;
} br_schedule_workspace;
typedef struct br_schedule {
    br_schedule_io io;
    const br_schedule_task *tasks; size_t count;
    char script[129],revision[65];
    br_schedule_record *records; size_t capacity;
    br_schedule_history *history; size_t history_count,history_capacity;
    br_schedule_workspace *workspace;
    uint64_t sequence,owned,mono,replans;
    int64_t wall;
    int blocked,trusted,sampled,reconciled,idle;
    unsigned schema;
    const struct br_schedule_guard *guard; /* borrowed, NULL = isolated API */
} br_schedule;
/* init always reads original state. Corruption/config mismatch => blocked,
 * never empty fallback. Existing intent/running => unknown, saved before use.
 * New state saves binding/seeds before return. 1 success/-1 blocked/invalid. */
int br_schedule_init(br_schedule *,br_schedule_io,const char *script,const char *revision,
                     const br_schedule_task *,size_t,br_schedule_record *,size_t capacity,
                     br_schedule_workspace *);
/* Optional borrowed retirement storage, capacity 0..64. Reads schema1 without
 * rewriting it. Schema2 with more history than capacity blocks, never drops.
 * Original init is the history=NULL/capacity=0 compatibility wrapper. */
int br_schedule_init_history(br_schedule *,br_schedule_io,const char *script,const char *revision,
                     const br_schedule_task *,size_t,br_schedule_record *,size_t capacity,
                     br_schedule_history *,size_t history_capacity,br_schedule_workspace *);
/* Validate borrowed mutable storage and cross-instance non-aliasing. Shared
 * workspace is allowed, overlapping records/state/config are not. */
int br_schedule_storage_valid(const br_schedule *);
int br_schedule_storage_separate(const br_schedule *,const br_schedule *);
/* Readonly original codec observation. No calendar is invented for archived
 * current records: calendar_verified=0 unless supplied original tasks match;
 * a zero-task archive is fully verifiable. Counts include disabled tasks.
 * Outputs publish only after exact full decoding; no write/recovery occurs. */
typedef struct {
    char script[129],revision[65];
    size_t task_count,history_count,payload_bytes;
    uint64_t sequence;
    unsigned schema,active_count,terminal_count,calendar_verified;
} br_schedule_observation;
int br_schedule_probe_bytes(const void *,size_t,const br_schedule_task *optional_tasks,
                             size_t count,br_schedule_observation *);
int br_schedule_probe_io(br_schedule_io,br_schedule_workspace *,const br_schedule_task *optional_tasks,
                          size_t count,br_schedule_observation *);
/* Exact original encoder measurement, no scratch/busy/live-state mutation. */
int br_schedule_measure(const br_schedule *,size_t *payload_bytes);
typedef struct {
    size_t task_count,history_count,payload_bytes;
    unsigned schema;
} br_schedule_projection;
enum br_schedule_change { BR_SCHEDULE_EDIT=1,BR_SCHEDULE_CLAIM=2,BR_SCHEDULE_AUTH=3 };
typedef struct br_schedule_guard {
    void *context;
    /* Before original RAM mutation; strict1 permits only this synchronous
     * call. No IO callback reentry, ownership/proof creation or delayed permit. */
    int (*before_change)(void *,const br_schedule *,unsigned,
                         const br_schedule_projection *,const char *new_revision,unsigned original_slot);
    /* Original shared workspace has been erased and busy dropped. */
    int (*after_save)(void *,const br_schedule *,int);
    /* Only called AFTER original release checked its original proof arguments. */
    void (*after_release)(void *,const br_schedule *);
} br_schedule_guard;
int br_schedule_guard_bind(br_schedule *,const br_schedule_guard *);
int br_schedule_guard_unbind(br_schedule *,const br_schedule_guard *);
/* Same original mapping/validation as reconfigure, without mutation/IO.
 * 1 projected, 0 owned/active, -1 invalid/blocked. This is a measurement,
 * not a delayed write permit: caller must retain the same serial lease. */
int br_schedule_reconfigure_preflight(const br_schedule *,const char *expected_revision,
                         const char *new_revision,const br_schedule_task *,size_t,
                         br_schedule_projection *);
/* Preserve seed/highwater/auth by ID, reset pending plans. Add/remove requires
 * history capacity>0; removal retains summary, re-add restores it. New IDs
 * require nonzero seed. Capacity/full/alias/invalid => -1 before mutation.
 * 1 durable success, 0 active/owned: original config MUST stay alive, -1 invalid
 * or blocked. First set change writes schema2. No history eviction. Write failure blocks;
 * reopen original/new revision from disk, never continue ambiguous RAM.
 * Requires no runner borrowing an active run/candidate; re-register afterwards.
 * Success clears reconciliation/time sample; new caps + task.list mandatory. */
int br_schedule_reconfigure(br_schedule *,const char *expected_revision,const char *new_revision,
                            const br_schedule_task *,size_t);
/* Explicit owner proof: authenticated CURRENT caps + task.list recovery pass.
 * Not proof that any unknown action did not execute. Unknown still blocks. */
int br_schedule_reconciled(br_schedule *);
/* Returns slot+1 ready for claim, 0 waiting/busy/clock/recovery, -1 blocked.
 * idle strict bool must cover official/dialogue/player/window/ring/unknown
 * owners. This sample must also be the claim sample; claim obtains no owner. */
int br_schedule_poll(br_schedule *,int trusted,int64_t wall,uint64_t mono,int idle);
/* Save intent FIRST; 1 yields a send-once serial in record, 0 no permission,
 * -1 invalid/blocked. Caller atomically acquires A idle owner before claim,
 * then validates current caps + scheduled/silent policy before actual send.
 * Failure after intent is UNKNOWN unless owner proves no OPEN was sent. */
int br_schedule_claim(br_schedule *,unsigned slot,uint64_t mono,uint64_t epoch,const char *uuid);
int br_schedule_ack(br_schedule *,uint64_t serial,uint64_t epoch,const char *uuid,const char *remote);
/* no_send: ONLY proven zero business bytes sent/no side effects. 30/60/120/
 * 300s retries, grace still applies. auth=1 pauses until explicit auth reset.
 * Otherwise marks unknown. ACKed work can never be returned to pending. */
int br_schedule_failed(br_schedule *,uint64_t serial,uint64_t mono,int no_send,int auth);
int br_schedule_auth_reset(br_schedule *,unsigned slot);
/* proof=1 means original identity/version + terminal Ledger/K6 durable (or
 * corresponding synchronous/flow terminal) verified by owner. No TEXT/ACK.
 * State stores identity, not result prose; K6 owns receipt/text/announcement. */
int br_schedule_finish(br_schedule *,uint64_t serial,uint64_t epoch,const char *uuid,int proof);
/* Only after real owned resource drain, even if terminal was already saved. */
int br_schedule_release(br_schedule *,uint64_t serial,int reaped);
/* Exact readonly disk comparison of the original owned TERMINAL snapshot.
 * Reuses the original codec/workspace; no new allocation, decode, write or
 * release. Missing/changed/ambiguous disk blocks until genuine reopen.
 * 1 exact; -1 invalid/blocked. Caller keeps original configuration immutable. */
int br_schedule_terminal_current(br_schedule *,uint64_t serial,uint64_t epoch,const char *command);
/* Read-only exact original codec comparison; no ownership or send authority.
 * Retired variant requires unowned retained TERMINAL before calendar replans. */
int br_schedule_state_current(br_schedule *);
int br_schedule_retired_current(br_schedule *,uint64_t serial,uint64_t epoch,const char *command);
/* Exact unowned original no-send PENDING before explicit new claim. Does not
 * waive original backoff/grace/auth/idle/caps rules or authorize replay. */
int br_schedule_deferred_current(br_schedule *,uint64_t serial,uint64_t epoch,const char *command);
#endif
