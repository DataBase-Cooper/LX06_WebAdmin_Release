#include "opus_codec.h"

#include <string.h>

static int supported_rate(int rate) {
    return rate == 16000 || rate == 24000;
}

int xz_opus_codec_init(xz_opus_codec *codec, int input_rate, int output_rate) {
    if (!codec || !supported_rate(input_rate) || !supported_rate(output_rate)) return -1;
    memset(codec, 0, sizeof(*codec));

    int error = OPUS_OK;
    codec->encoder = opus_encoder_create(input_rate, 1, OPUS_APPLICATION_VOIP, &error);
    if (!codec->encoder || error != OPUS_OK) {
        xz_opus_codec_destroy(codec);
        return -1;
    }
    if (opus_encoder_ctl(codec->encoder, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
        opus_encoder_ctl(codec->encoder, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
        opus_encoder_ctl(codec->encoder, OPUS_SET_INBAND_FEC(1)) != OPUS_OK) {
        xz_opus_codec_destroy(codec);
        return -1;
    }

    codec->decoder = opus_decoder_create(output_rate, 1, &error);
    if (!codec->decoder || error != OPUS_OK) {
        xz_opus_codec_destroy(codec);
        return -1;
    }
    codec->input_rate = input_rate;
    codec->output_rate = output_rate;
    return 0;
}

int xz_opus_encode(xz_opus_codec *codec, const int16_t *pcm, size_t samples,
                   unsigned char *packet, size_t capacity) {
    if (!codec || !codec->encoder || !pcm || !packet || capacity == 0 ||
        capacity > (size_t)INT32_MAX ||
        samples != (size_t)codec->input_rate * 60u / 1000u ||
        samples > (size_t)INT32_MAX) return -1;
    return opus_encode(codec->encoder, (const opus_int16 *)pcm, (int)samples,
                       packet, (opus_int32)capacity);
}

int xz_opus_decode(xz_opus_codec *codec, const unsigned char *packet,
                   size_t packet_length, int16_t *pcm, size_t capacity_samples) {
    if (!codec || !codec->decoder || !packet || packet_length == 0 || !pcm ||
        packet_length > (size_t)INT32_MAX ||
        capacity_samples < (size_t)codec->output_rate * 60u / 1000u ||
        capacity_samples > (size_t)INT32_MAX) return -1;
    return opus_decode(codec->decoder, packet, (opus_int32)packet_length,
                       (opus_int16 *)pcm, (int)capacity_samples, 0);
}

void xz_opus_codec_destroy(xz_opus_codec *codec) {
    if (!codec) return;
    if (codec->encoder) opus_encoder_destroy(codec->encoder);
    if (codec->decoder) opus_decoder_destroy(codec->decoder);
    memset(codec, 0, sizeof(*codec));
}
