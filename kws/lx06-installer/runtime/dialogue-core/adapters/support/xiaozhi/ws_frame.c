#include "ws_frame.h"

#include <stdint.h>
#include <string.h>

static int valid_opcode(unsigned opcode) {
    return opcode == 0 || opcode == 1 || opcode == 2 || opcode == 8 || opcode == 9 || opcode == 10;
}

int dc_xz_ws_build_client_frame(int opcode, const unsigned char *payload, size_t payload_length,
                             const unsigned char mask[4], unsigned char *out,
                             size_t out_capacity, size_t *out_length) {
    if (!out || !out_length || !mask || (payload_length && !payload) ||
        !valid_opcode((unsigned)opcode)) return -1;
    if ((opcode & 8) && payload_length > 125) return -1;

    size_t header = payload_length < 126 ? 6 : payload_length <= 65535 ? 8 : 14;
    if (payload_length > SIZE_MAX - header || out_capacity < header + payload_length) return -1;
    size_t i = 0;
    out[i++] = (unsigned char)(0x80u | (unsigned)opcode);
    if (payload_length < 126) {
        out[i++] = (unsigned char)(0x80u | payload_length);
    } else if (payload_length <= 65535) {
        out[i++] = 0xfe;
        out[i++] = (unsigned char)(payload_length >> 8);
        out[i++] = (unsigned char)payload_length;
    } else {
        out[i++] = 0xff;
        uint64_t n = (uint64_t)payload_length;
        for (int shift = 56; shift >= 0; shift -= 8) out[i++] = (unsigned char)(n >> shift);
    }
    memcpy(out + i, mask, 4);
    i += 4;
    for (size_t n = 0; n < payload_length; ++n) out[i + n] = payload[n] ^ mask[n & 3];
    *out_length = i + payload_length;
    return 0;
}

int dc_xz_ws_decode_frame(const unsigned char *input, size_t input_length, int sender_is_client,
                       dc_xz_ws_frame *frame, unsigned char *payload_out,
                       size_t payload_capacity, size_t *payload_length) {
    if (!input || !frame || !payload_out || !payload_length || input_length < 2) return -1;
    unsigned char b0 = input[0], b1 = input[1];
    if ((b0 & 0x70) != 0 || !valid_opcode(b0 & 0x0f)) return -1;
    int fin = (b0 & 0x80) != 0;
    int masked = (b1 & 0x80) != 0;
    if (masked != (sender_is_client != 0)) return -1;

    uint64_t n = b1 & 0x7f;
    size_t pos = 2;
    if (n == 126) {
        if (input_length - pos < 2) return -1;
        n = ((uint64_t)input[pos] << 8) | input[pos + 1];
        pos += 2;
        if (n < 126) return -1;
    } else if (n == 127) {
        if (input_length - pos < 8 || (input[pos] & 0x80)) return -1;
        n = 0;
        for (int j = 0; j < 8; ++j) n = (n << 8) | input[pos++];
        if (n <= 65535) return -1;
    }
    if ((b0 & 0x08) && (!fin || n > 125)) return -1;
    if (n > SIZE_MAX || n > payload_capacity) return -1;
    size_t length = (size_t)n;
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (input_length - pos < 4) return -1;
        memcpy(mask, input + pos, 4);
        pos += 4;
    }
    if (input_length - pos != length) return -1;
    for (size_t j = 0; j < length; ++j)
        payload_out[j] = input[pos + j] ^ (masked ? mask[j & 3] : 0);

    frame->fin = fin;
    frame->opcode = b0 & 0x0f;
    frame->masked = masked;
    frame->payload_length = length;
    *payload_length = length;
    return 0;
}
