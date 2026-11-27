/* Private speaker state API 1, PROVISIONAL(P-20261111-0749-B).
 * A supplies an already authorized private directory FD. No path discovery,
 * mkdir, chmod, settings storage, public logging, or phone Ledger here.
 * Caller serializes access. Keep the writer lock for the entire inbox lifetime.
 */
#ifndef LX06_BR_INBOX_STORE_H
#define LX06_BR_INBOX_STORE_H
#include <stddef.h>
#define BR_STORE_LIMIT (192u * 1024u)
enum br_store_kind { BR_STORE_INBOX=0,BR_STORE_SCHEDULE=1,BR_STORE_FLOW=2,BR_STORE_OWNER=3 };
typedef struct { size_t payload_bytes,framed_bytes; unsigned kind; } br_store_observation;
struct br_store;
typedef struct br_store_guard {
    void *context;
    /* strict1 before any original temp/write; failure changes no store bytes. */
    int (*before_write)(void *,const struct br_store *,const void *,size_t);
    /* Called after original write returns; never reuse a busy caller workspace. */
    int (*after_write)(void *,const struct br_store *,int);
    void (*closing)(void *,const struct br_store *);
} br_store_guard;
typedef struct br_store {
    int directory,lock,poisoned,schedule;
    const br_store_guard *guard; /* borrowed; NULL preserves isolated API */
} br_store;
int br_store_guard_bind(br_store *,const br_store_guard *);
int br_store_guard_unbind(br_store *,const br_store_guard *);
/* Directory must be owned by euid, mode 0700. Duplicates FD, never owns input. */
int br_store_open(br_store *,int directory);
/* Fixed scheduler namespace in the SAME authorized directory, separate lock,
 * magic, data and temp. No arbitrary filename API. Limit 16KiB.
 * PROVISIONAL(P-20261111-1343-B); original inbox format/API unchanged. */
int br_store_open_schedule(br_store *,int directory);
/* Single global flow log, fixed namespace, 40KiB payload. No reset/path API.
 * PROVISIONAL(P-20261111-1632-B). */
int br_store_open_flow(br_store *,int directory);
/* Original-owner identity handoff only, SAME fixed namespace, max7480B payload
 * (legacy7388B; scheduled envelope7480B, PROVISIONAL(P-20261112-1610-B)).
 * Old codecs keep rejecting the scheduled image. No new registry or Ledger. */
int br_store_open_owner(br_store *,int directory);
void br_store_close(br_store *);
/* read: 1 valid bytes, 0 absent, -1 IO/corrupt/unsafe. No empty fallback.
 * write: 0 durable, -1 failed. After rename + failed directory fsync the disk
 * outcome is ambiguous: poisoned=1, all operations fail until close/reopen.
 * A checksum detects damage; it does not authenticate untrusted writers.
 */
int br_store_read(br_store *,void *,size_t capacity,size_t *length);
int br_store_write(br_store *,const void *,size_t length);
/* Readonly original frame/CRC validation, no create/unlink/cleanup/recovery.
 * Existing original writer must use observe_owned; observe_at borrows a
 * shared lock on an EXISTING private lock file and refuses a live writer.
 * Missing data+lock returns0; data without lock, any temp/poison/unsafe
 * identity returns-1. Outputs publish only on1. Payload is caller scratch. */
/* NULL payload/capacity0 observes full original frame using bounded streaming
 * CRC validation, without retaining state bytes. Ordinary read is unchanged. */
int br_store_observe_at(int authorized_directory,unsigned kind,void *,size_t,
                        br_store_observation *);
int br_store_observe_owned(br_store *,void *,size_t,br_store_observation *);
#endif
