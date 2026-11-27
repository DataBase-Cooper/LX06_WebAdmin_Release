#ifndef XZ_WS_FRAME_H
#define XZ_WS_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fin;
    int opcode;
    int masked;
    size_t payload_length;
} xz_ws_frame;

int xz_ws_build_client_frame(int opcode, const unsigned char *payload, size_t payload_length,
                             const unsigned char mask[4], unsigned char *out,
                             size_t out_capacity, size_t *out_length);
int xz_ws_decode_frame(const unsigned char *input, size_t input_length, int sender_is_client,
                       xz_ws_frame *frame, unsigned char *payload_out,
                       size_t payload_capacity, size_t *payload_length);

#endif
