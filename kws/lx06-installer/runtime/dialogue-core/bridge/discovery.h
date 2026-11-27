/* T16/T17 isolated discovery consumer. PROVISIONAL(P-20261111-1148-B).
 * Serial, caller-clocked, no sockets/TLS, audio, settings or disk operations.
 * This discovery-only consumer cannot carry an active business/audio session.
 * Not connected to the legacy probe or production profile before T14.
 */
#ifndef LX06_BR_DISCOVERY_H
#define LX06_BR_DISCOVERY_H
#include "wire.h"
#include "../../common/bridge-contract/caps.h"
#include "../../common/bridge-contract/describe.h"
enum br_discovery_event { BR_DISC_NONE=0,BR_DISC_CAPS=1,BR_DISC_DESCRIPTION=2,
    BR_DISC_HEARTBEAT=3,BR_DISC_ACK=4,BR_DISC_PROGRESS=5,BR_DISC_DIAGNOSTIC=6,
    BR_DISC_INVALID=-1,BR_DISC_CLOSED=-2,BR_DISC_DESCRIBE_FAILED=-3 };
enum br_discovery_phase { BR_DISC_OFF=0,BR_DISC_DIALING=1,BR_DISC_WAIT_CAPS=2,BR_DISC_READY=3 };
typedef struct {
    br_caps caps;
    br_json description,scratch;
    dc_wire_packet packet;
    uint64_t epoch,now_ms,started_ms,heartbeat_ms,describe_ms;
    /* P-20261112-0001-B: monotonic local probe serial << 1 | awaiting PONG.
     * Nonce = original epoch LE64 + serial LE64. No borrowed nonce buffer. */
    uint64_t probe;
    /* P-20261111-2300-B: app string token in our immutable current caps,
     * 0 = all apps. No params copy or pointer into ephemeral scratch.
     * External caps mutation is forbidden through describe/session lifetime. */
    unsigned selector_app;
    const char *error; /* fixed static existing wire error name, or NULL */
    int phase,describing,describe_failed,has_description;
    int business_used; /* P-1207: one business OPEN per epoch, across re-init */
} br_discovery;
/* Allocate outside the real-time stack. Treat internals as opaque, except
 * phase/error/has_description and const caps/description inspection. Never
 * mutate embedded caps to install a saved snapshot or descriptions. Input
 * buffers must not overlap state. A new begin requires a fresh epoch chosen
 * by the sole owner (this consumer does not generate identities).
 */
int br_discovery_begin(br_discovery *,uint64_t epoch,uint64_t now_ms);
/* Caller invokes ONLY after TLS>=1.2 + CA/SPKI + Bearer succeeds. This API
 * does not attest authentication. On success caller must send caps.request
 * with the sole owner's complete OPEN envelope, or close on send failure.
 */
int br_discovery_authenticated(br_discovery *,uint64_t epoch,uint64_t now_ms);
int br_discovery_poll(br_discovery *,uint64_t now_ms);
void br_discovery_close(br_discovery *);
/* Validated WebSocket CONTROL pong for the owner's CURRENT control probe
 * only. Caller must verify its own nonce, not arbitrary ping/pong RX, Wire,
 * hook IPC, GRANT, TEXT or progress. Wire probes use the APIs below.
 */
int br_discovery_heartbeat(br_discovery *,uint64_t epoch,uint64_t now_ms);
/* Local API2, PROVISIONAL(P-20261112-0001-B), implementing approved M16.
 * Encode a fresh 16B opaque Wire PING in WAIT_CAPS/READY. New probe replaces
 * previous expectation without extending heartbeat/deadlines; counter never
 * wraps. 1 success, negative refusal. Output/length unchanged on refusal.
 * Send once; failed/uncertain send MUST close original connection. 64B output
 * suffices; disjoint buffers and serial owner, no callbacks/heap. */
int br_discovery_probe(br_discovery *,uint64_t epoch,uint64_t now_ms,
                       void *out,size_t capacity,size_t *length);
/* Internal shared receive tail, only AFTER original br_wire_decode validated
 * s->packet. PING returns HEARTBEAT to request an echo but DOES NOT renew
 * liveness. Current matching PONG renews once; all other PONG return NONE.
 * Owner uses br_wire_pong on original bytes with a separate/send workspace.
 * This is transport data only, never business/audio/result authority. */
int br_discovery_wire_heartbeat(br_discovery *,uint64_t now_ms);
/* After success send the independent hook.describe OPEN; send failure closes
 * transport. No same-epoch retry after timeout/refusal (late state ambiguity).
 */
int br_discovery_describe_begin(br_discovery *,uint64_t epoch,uint64_t now_ms,
                               const void *params,size_t length);
/* Complete authenticated binary WS message from PHONE. Structural failures
 * close; invalid description ends only its request, preserving current caps.
 * packet/scratch are erased before return; no received text is logged.
 * READY caps/description events never mean business operation/audio READY.
 */
int br_discovery_receive(br_discovery *,uint64_t epoch,uint64_t now_ms,const void *,size_t);
#endif
