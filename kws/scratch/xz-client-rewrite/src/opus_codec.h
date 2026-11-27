#ifndef XZ_OPUS_CODEC_H
#define XZ_OPUS_CODEC_H

#include <opus/opus.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    OpusEncoder *encoder;
    OpusDecoder *decoder;
    int input_rate;
    int output_rate;
} xz_opus_codec;

int xz_opus_codec_init(xz_opus_codec *codec, int input_rate, int output_rate);
int xz_opus_encode(xz_opus_codec *codec, const int16_t *pcm, size_t samples,
                   unsigned char *packet, size_t capacity);
int xz_opus_decode(xz_opus_codec *codec, const unsigned char *packet,
                   size_t packet_length, int16_t *pcm, size_t capacity_samples);
void xz_opus_codec_destroy(xz_opus_codec *codec);

#endif
