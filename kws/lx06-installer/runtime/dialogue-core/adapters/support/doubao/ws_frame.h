#ifndef DC_DB_WS_FRAME_H
#define DC_DB_WS_FRAME_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fin;
    int opcode;
    int masked;
    size_t payload_length;
} dc_db_ws_frame;

int dc_db_ws_build_client_frame(int opcode, const unsigned char *payload, size_t payload_length,
                             const unsigned char mask[4], unsigned char *out,
                             size_t out_capacity, size_t *out_length);
int dc_db_ws_decode_frame(const unsigned char *input, size_t input_length, int sender_is_client,
                       dc_db_ws_frame *frame, unsigned char *payload_out,
                       size_t payload_capacity, size_t *payload_length);

#endif
