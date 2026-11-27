/* T20/K6 API 1. PROVISIONAL(P-20261111-1225-B).
 * Serial adapter from the isolated operation consumer to the existing inbox.
 * No phone Ledger, settings parser, sockets, TTS, automatic retry or logging.
 */
#ifndef LX06_BR_RESULT_DELIVERY_H
#define LX06_BR_RESULT_DELIVERY_H
#include "operation.h"
#include "result_inbox.h"
enum br_delivery_rc { BR_DELIVERY_IGNORED=0,BR_DELIVERY_COMPLETE=1,
    BR_DELIVERY_DEFERRED=2,BR_DELIVERY_INVALID=-1,BR_DELIVERY_IO=-2 };
typedef struct {
    unsigned total,durable,accepted,duplicates,stopped_at;
    int last_rc; /* br_received, or 0 if no row attempted; no received prose */
} br_delivery_report;
typedef struct {
    br_delivery_report report;
    br_result scratch; /* allocate this state outside the real-time stack */
    uint16_t records[BR_INBOX_ROWS];
} br_delivery;
/* A/owner resolves ORIGINAL task output policy, never a result-supplied flag.
 * Return exactly 1 and set audible to 0/1, ttl_s to 60..86400. Called per row,
 * including duplicates; missing policy/association must never enable audio.
 */
typedef int (*br_delivery_policy)(void *,const br_result *,int *audible,unsigned *ttl_s);
/* Shared shape decoder, no provenance/association/IO. Row must come from the
 * authenticated TASK/task.list path. Clears out on failure. */
int br_delivery_decode(const br_json *,unsigned row,br_result *out);
/* op/discovery/inbox outlive call; buffers/state disjoint and serial.
 * Only most recent BR_OP_TASK (kind=result) or BR_OP_TASKS accepted, with
 * current live discovery/caps. Authenticated provenance is the transport
 * owner's responsibility. TASK uses current epoch; list preserves history.
 * Prevalidates all row shapes before IO; association/policy/store failure can
 * leave a durable prefix. Report gives that prefix. Retry full response or
 * re-fetch Ledger; duplicate is durable. No batch transaction or online ACK.
 * No source json fields are persisted here; private op.response retains them
 * until next prepare/receive/abort. Success means receipt, never playback or
 * flow success. clock_trusted is strict 0/1, now is trusted Unix milliseconds.
 */
int br_delivery_accept(br_delivery *,const br_operation *,br_inbox *,uint64_t now_ms,
                       int clock_trusted,br_delivery_policy,void *context);
#endif
