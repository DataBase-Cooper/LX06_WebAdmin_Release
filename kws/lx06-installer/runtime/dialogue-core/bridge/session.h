/* Local API7, PROVISIONAL(P-20261111-2004-B/P-20261111-2315-B/P-20261112-1851-B/P-20261112-2025-B/P-20261112-2053-B/P-20261112-2136-B/P-20261112-2214-B). Serial OPEN router.
 * Isolated until T14/A/C signoff. No TLS/sockets/audio/settings/phone Ledger.
 * Fresh init only; sole serial owner, immutable disjoint buffers, no reentry.
 * Discovery and caps outlive session. Allocate state off realtime stack. */
#ifndef LX06_BR_SESSION_H
#define LX06_BR_SESSION_H
#include "operation.h"
#define BR_SESSION_LIMIT 32u
typedef struct {
    void *context;
    /* Exact original K6 durable receipt / actual owned-resource release.
     * Strict 1=proved, 0=not yet, anything else=failure. No ACK fabrication. */
    int (*receipt)(void *,uint64_t run,uint64_t epoch,const char *command,const char *task);
    int (*reaped)(void *,uint64_t run,uint64_t epoch,const char *command,const char *task);
    void (*close)(void *,uint64_t run,uint64_t epoch); /* request, NOT reap proof */
} br_session_io;
/* Private in-memory layout; rebuild every consumer together. Not a durable
 * receipt or Ledger. Fingerprints live until original session is replaced. */
typedef struct {
    char command[37],task[129];uint64_t generation;
    unsigned char result_hash[32];unsigned result_type;
    unsigned char grant_hash[32];unsigned grant_seen;
} br_session_ticket;
typedef struct {
    br_operation operation; /* common parser/response; inspect const only */
    br_session_io io;
    br_session_ticket tickets[BR_SESSION_LIMIT];
    uint64_t run,last_run,epoch,last_epoch,generation;
    char script[129];
    unsigned count;
    int live,owned,terminal,ended,busy;
    const char *error;
} br_session;
void br_session_init(br_session *); /* fresh storage only, never reset live */
/* Claims unused authenticated READY discovery, requires session.command_id.
 * Closed owned tickets MUST be released via original strict reap first.
 * 1 accepted, -1 refused without consuming discovery. No saved caps. */
int br_session_begin(br_session *,br_discovery *,uint64_t run,uint64_t now,br_session_io);
/* Same BR_OP_* events. Successful encode consumes UUID: send ONCE, close on
 * failure/uncertainty. One owned command until terminal+explicit release.
 * generation is current verified fence generation, NEVER audio authority. */
int br_session_prepare(br_session *,uint64_t run,uint64_t epoch,uint64_t generation,uint64_t now,
    const void *open,size_t,void *wire,size_t capacity,size_t *length);
/* Sole transport owner's nonblocking complete-message submit. Strict 1 means
 * copied/accepted locally, NOT delivered/ACK/READY. Any other result is send
 * uncertainty: close original ticket, never retry. Pointer borrowed ONLY for
 * callback; no direct discovery mutation/reset or calls to another owner.
 * Actual TLS>=1.2/CA+SPKI/Bearer/WSS checks remain the transport's obligation. */
typedef int (*br_session_send_io)(void *,uint64_t run,uint64_t epoch,const void *,size_t);
/* Shared prepare -> ONE submit -> erase encoded workspace. No new state/queue.
 * 1 local submit, otherwise original BR_OP error. run/epoch/generation/open
 * follow prepare; workspace capacity 1..8240, disjoint from input/state.
 * Invalid callback/workspace refused BEFORE clock/identity consumption.
 * Send callback cannot reenter session. Failed/uncertain send retains owned
 * original ticket for UNKNOWN/reap; success does not mark terminal or release.
 * Caller must not also send/retry these bytes via a second path. */
int br_session_send(br_session *,uint64_t run,uint64_t epoch,uint64_t generation,uint64_t now,
    const void *open,size_t,void *workspace,size_t capacity,br_session_send_io,void *context);
/* GRANT/READY/STATE/ERROR require exact command+generation, task bound at
 * GRANT or READY. Same bound GRANT reports ACK once; duplicate returns0.
 * Before terminal/retired controls drop=0, task binding must still match;
 * GRANT always validates original state=started via the shared consumer.
 * A retired unbound ticket cannot acquire a new task. This checks control
 * identity/GRANT semantics. READY/ERROR terminal replay must match the first
 * validated terminal type and entire canonical strict JSON fingerprint;
 * conflicting/unobserved terminal closes with unsupported, retaining owned.
 * Object order/whitespace/string escaping do not change a fingerprint;
 * arrays/types/all extension fields do. GRANT uses the same entire-payload
 * comparison after its first validated observation. READY before GRANT may
 * establish a late GRANT comparison baseline, but emits no ACK or authority.
 * STATE is mutable op progress, never a terminal/immutable-ACK fingerprint.
 * This is NOT a durable receipt, authentication or UNKNOWN resolution.
 * TASK unchanged to K6, never
 * implicitly completes pending. response expires on next current API call.
 * AUDIO/CANCEL/RESUMED routed separately by sole transport owner to fence. */
/* Wire opaque heartbeats use discovery's sole pending nonce; HEARTBEAT on
 * PING requests echo, not liveness/command completion. See operation.h. */
int br_session_receive(br_session *,uint64_t run,uint64_t epoch,uint64_t now,const void *,size_t);
/* oneway only: original associated K6 receipt callback, strict proof.
 * ledger/release recheck original READY epoch/current session.command_id caps
 * and no describe AFTER proof returns, before adopting even 0 or strict1.
 * Loss closes original run/epoch, retaining owned identity; no terminal or
 * next-OPEN permission. Replacement discovery remains untouched. Existing
 * durable facts do not roll back; UNKNOWN still uses original Ledger only.
 * No layout change, retries or new deadline; callbacks remain nonreentrant. */
int br_session_ledger(br_session *,uint64_t run,uint64_t epoch,uint64_t now);
/* terminal + strict reaped; 1 next OPEN allowed, 2 caps session=end, 0 busy,
 * BR_OP_CLOSED on proof-time authority loss, retaining owned. Other errors
 * keep original BR_OP codes. end never guessed from audio/action names. */
int br_session_release(br_session *,uint64_t run,uint64_t epoch,uint64_t now);
int br_session_poll(br_session *,uint64_t run,uint64_t epoch,uint64_t now);
int br_session_close(br_session *,uint64_t run,uint64_t epoch);
/* Closed original session only, current last ticket index/generation exact.
 * Reuses ORIGINAL io.reaped(command/task), strict 1 proof, never close again.
 * 1 released, 0 pending, -1 refused; non-1 proof preserves ownership.
 * No discovery mutation: a replacement connection is never released here. */
int br_session_release_closed(br_session *,uint64_t run,uint64_t epoch,
                              unsigned index,uint64_t generation);
/* Explicit exact-ticket close by sole owner, including detached discovery.
 * Closes only original run/epoch via io.close; replacement discovery intact.
 * Unlike stale poll/close callbacks, requires current owned ticket identity. */
int br_session_close_ticket(br_session *,uint64_t run,uint64_t epoch,
                            unsigned index,uint64_t generation);
#endif
