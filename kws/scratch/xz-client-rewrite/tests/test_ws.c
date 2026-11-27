#include "../src/ws_frame.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_client_frame_masks_payload_and_uses_binary_opcode(void) {
    const unsigned char data[] = {0x00, 0x7f, 0x80, 0xff};
    const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
    unsigned char frame[64], decoded[16];
    size_t frame_size = 0, decoded_size = 0;
    xz_ws_frame parsed;
    assert(xz_ws_build_client_frame(0x2, data, sizeof(data), mask,
                                    frame, sizeof(frame), &frame_size) == 0);
    assert((frame[1] & 0x80) != 0);
    assert(xz_ws_decode_frame(frame, frame_size, 1, &parsed,
                              decoded, sizeof(decoded), &decoded_size) == 0);
    assert(parsed.opcode == 0x2 && parsed.fin);
    assert(decoded_size == sizeof(data));
    assert(memcmp(decoded, data, sizeof(data)) == 0);
}

static void test_server_extended_length_and_mask_validation(void) {
    unsigned char frame[140], decoded[128];
    size_t decoded_size = 0;
    xz_ws_frame parsed;
    frame[0] = 0x82;
    frame[1] = 126;
    frame[2] = 0;
    frame[3] = 126;
    memset(frame + 4, 'x', 126);
    frame[4] = 'a'; frame[5] = 'b'; frame[6] = 'c';
    assert(xz_ws_decode_frame(frame, 130, 0, &parsed, decoded,
                              sizeof(decoded), &decoded_size) == 0);
    assert(decoded_size == 126 && memcmp(decoded, "abc", 3) == 0);

    frame[1] |= 0x80; /* servers must not mask */
    assert(xz_ws_decode_frame(frame, 11, 0, &parsed, decoded,
                              sizeof(decoded), &decoded_size) != 0);
}

int main(void) {
    test_client_frame_masks_payload_and_uses_binary_opcode();
    test_server_extended_length_and_mask_validation();
    puts("websocket frame tests: PASS");
    return 0;
}
