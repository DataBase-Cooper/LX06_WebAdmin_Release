/* T16 isolated phone framing. No WSS/TLS listener, JSON/settings parser,
 * device IO, or production protocol.c changes. Caller supplies one complete
 * binary WebSocket message, after its existing TLS/CA/SPKI/token checks.
 */
#ifndef LX06_BR_WIRE_H
#define LX06_BR_WIRE_H
#include "../protocol.h"
#include "../../common/bridge-contract/versions.h"
enum br_direction { BR_FROM_SPEAKER=1, BR_FROM_PHONE=2 };
enum br_wire_rc { BR_WIRE_INVALID=-1, BR_WIRE_VERSION=-2, BR_WIRE_OK=1 };
/* Semantic validation is mandatory before exposing any control packet. Reuse
 * K0/K3 schemas; this callback is NOT readiness, grant or liveness evidence.
 * Return exactly 1 to accept. PCM is structurally checked without JSON.
 * Pointer borrowed only during call; do not log private payloads.
 */
typedef int (*br_payload_check)(void *,const dc_wire_header *,const unsigned char *,size_t);
int br_wire_type_allowed(unsigned type,enum br_direction);
/* Explicit LE codec, independent of host alignment/endian. Output remains
 * unchanged on refusal. decode copies into packet (no borrowed message).
 * live_epoch must match header; historical task.list rows keep their own epoch
 * inside JSON and are independently checked by K6, never relabelled here.
 */
int br_wire_decode(const void *,size_t,enum br_direction,uint64_t live_epoch,
                   br_payload_check,void *context,dc_wire_packet *out);
int br_wire_encode(const dc_wire_packet *,enum br_direction,uint64_t live_epoch,
                   br_payload_check,void *context,void *out,size_t capacity,size_t *length);
/* M16: opaque Wire heartbeat, never JSON/UTF-8/credentials/IPC timestamps.
 * Control flags/sequence must be zero; normal codec epoch/rate checks apply. */
int br_wire_heartbeat_type(unsigned type);
int br_wire_heartbeat_check(void *,const dc_wire_header *,const unsigned char *,size_t);
/* Pure speaker PONG encoder. Validate an original PHONE PING and echo every
 * payload byte plus epoch/generation; no liveness update. Workspace is erased
 * on every return. Input/state/output disjoint, output/length unchanged on
 * refusal. No send: sole transport owner sends once or closes on uncertainty. */
int br_wire_pong(const void *,size_t,uint64_t epoch,dc_wire_packet *workspace,
                  void *out,size_t capacity,size_t *length);
#endif
