/* Local, bounded transport-adapter ABI. No microphone, player or light API is
 * exposed to adapters. Credentials may cross this private inherited socket;
 * payloads must never be logged. Both endpoints are local little-endian builds.
 */
#ifndef LX06_DC_PROTOCOL_H
#define LX06_DC_PROTOCOL_H
#include "types.h"
#include <stddef.h>
#include <stdint.h>
#define DC_WIRE_MAGIC UINT32_C(0x31474344)
#define DC_WIRE_VERSION 1u
#define DC_WIRE_CAP 8192u
#define DC_ADAPTER_FD 3
enum dc_wire_type {
    DC_CMD_OPEN = 1, DC_CMD_MIC, DC_CMD_CANCEL, DC_CMD_RESUME,
    DC_CMD_CLOSE, DC_CMD_GRANT,
    /* Audio-provider greeting only. 7 is reserved; phone keeps commands1..6.
     * Core-owned generation, sequence0, nonempty UTF-8 text up to240 bytes. */
    DC_CMD_GREET = 8,
    DC_EVT_READY = 32, DC_EVT_USER_PARTIAL, DC_EVT_USER_FINAL,
    DC_EVT_TEXT, DC_EVT_REPLY_BEGIN, DC_EVT_AUDIO, DC_EVT_REPLY_END,
    DC_EVT_CANCEL_ACK, DC_EVT_ERROR, DC_EVT_RESUMED
};
enum dc_wire_flags {
    DC_WIRE_FINAL = 1u, DC_WIRE_PCM_S16LE = 2u,
    DC_WIRE_RETRYABLE = 4u
};
typedef struct dc_wire_header {
    uint32_t magic;
    uint16_t version, type;
    uint32_t length, flags;
    uint64_t epoch, generation, sequence;
    uint32_t rate;
    uint16_t channels, reserved;
} dc_wire_header;
typedef struct dc_wire_packet {
    dc_wire_header header;
    unsigned char payload[DC_WIRE_CAP];
} dc_wire_packet;
_Static_assert(sizeof(dc_wire_header) == 48, "wire ABI must be 48 bytes");
/* 0 success; -1 with errno (including EAGAIN). Never blocks. Send preserves a
 * caller's epoch/generation: old output must not be relabelled after CANCEL. */
int dc_wire_send(int fd, const dc_wire_header *, const void *payload);
/* 1 packet, 0 clean peer close, -1 error/EAGAIN. Rejects truncated packets,
 * unknown types/flags, malformed format, size, version and ancillary FDs. */
int dc_wire_receive(int fd, dc_wire_packet *);
dc_wire_header dc_wire_make(unsigned type, uint64_t epoch, uint64_t generation,
                            uint64_t sequence, size_t length);
#endif
