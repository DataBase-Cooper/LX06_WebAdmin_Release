#include "../src/opus_codec.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_v1_voice_frame_round_trips_at_60ms(void) {
    xz_opus_codec codec;
    assert(xz_opus_codec_init(&codec, 16000, 24000) == 0);

    int16_t input[960];
    for (size_t i = 0; i < 960; ++i) input[i] = (int16_t)((i % 80) * 300 - 12000);
    unsigned char packet[4000];
    int packet_length = xz_opus_encode(&codec, input, 960, packet, sizeof(packet));
    assert(packet_length > 0 && (size_t)packet_length <= sizeof(packet));

    int16_t output[1440];
    int decoded_samples = xz_opus_decode(&codec, packet, (size_t)packet_length,
                                         output, 1440);
    assert(decoded_samples == 1440);
    assert(memcmp(input, output, sizeof(input)) != 0);
    xz_opus_codec_destroy(&codec);
}

static void test_invalid_frame_lengths_are_rejected(void) {
    xz_opus_codec codec;
    assert(xz_opus_codec_init(&codec, 16000, 24000) == 0);
    int16_t input[959] = {0};
    unsigned char packet[4000];
    assert(xz_opus_encode(&codec, input, 959, packet, sizeof(packet)) < 0);
    xz_opus_codec_destroy(&codec);
}

int main(void) {
    test_v1_voice_frame_round_trips_at_60ms();
    test_invalid_frame_lengths_are_rejected();
    puts("opus codec tests: PASS");
    return 0;
}
