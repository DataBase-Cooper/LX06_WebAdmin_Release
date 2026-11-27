#include "../src/aec.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME 160
#define FRAMES 500
#define TOTAL (FRAME * FRAMES)

static uint32_t rng = 0x13579bdfu;
static int16_t noise_sample(void) {
    rng = rng * 1664525u + 1013904223u;
    return (int16_t)((int)(rng >> 16) - 32768);
}

static void test_aec_cancels_delayed_echo_without_erasing_double_talk(void) {
    xz_aec aec;
    assert(xz_aec_init(&aec, 16000, 128, 32, 0.12f) == 0);

    int16_t reference[TOTAL];
    int16_t user[TOTAL];
    int16_t microphone[TOTAL];
    int16_t residual[TOTAL];
    for (size_t i = 0; i < TOTAL; ++i) {
        reference[i] = (int16_t)(noise_sample() / 3);
        user[i] = (int16_t)(noise_sample() / 20);
    }
    for (size_t i = 0; i < TOTAL; ++i) {
        int32_t delayed = (i >= 32) ? reference[i - 32] : 0;
        int32_t mixed = delayed * 45 / 100 + user[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        microphone[i] = (int16_t)mixed;
    }

    for (int frame = 0; frame < FRAMES; ++frame) {
        size_t off = (size_t)frame * FRAME;
        assert(xz_aec_process(&aec, reference + off, microphone + off,
                              residual + off, FRAME) == 0);
    }

    double echo_in = 0.0, echo_out = 0.0, user_out = 0.0, user_res = 0.0;
    size_t begin = TOTAL - 5000;
    for (size_t i = begin; i < TOTAL; ++i) {
        double echo = (i >= 32) ? reference[i - 32] * 0.45 : 0.0;
        double e = residual[i];
        double u = user[i];
        echo_in += echo * echo;
        echo_out += e * e;
        user_out += u * u;
        user_res += e * u;
    }
    assert(echo_out < echo_in * 0.30);
    assert(user_res / sqrt(user_out * echo_out) > 0.70);

    xz_aec_destroy(&aec);
}

static void test_aec_reports_residual_reference_correlation(void) {
    xz_aec aec;
    assert(xz_aec_init(&aec, 16000, 64, 0, 0.0001f) == 0);
    int16_t reference[FRAME];
    int16_t microphone[FRAME];
    int16_t residual[FRAME];
    for (size_t i = 0; i < FRAME; ++i) {
        reference[i] = noise_sample();
        microphone[i] = (int16_t)(reference[i] / 2);
    }
    assert(xz_aec_process(&aec, reference, microphone, residual, FRAME) == 0);
    assert(aec.last_correlation > 0.90f);
    xz_aec_destroy(&aec);
}

static void test_aec_reports_residual_correlation_at_playback_delay(void) {
    xz_aec aec;
    assert(xz_aec_init(&aec, 16000, 64, 0, 0.0001f) == 0);
    int16_t reference[FRAME];
    int16_t microphone[FRAME];
    int16_t residual[FRAME];
    const size_t acoustic_delay = 24;
    for (size_t i = 0; i < FRAME; ++i) {
        reference[i] = noise_sample();
        microphone[i] = i >= acoustic_delay
            ? (int16_t)(reference[i - acoustic_delay] / 2) : 0;
    }
    assert(xz_aec_process(&aec, reference, microphone, residual, FRAME) == 0);
    assert(aec.last_correlation > 0.80f);
    xz_aec_destroy(&aec);
}

int main(void) {
    test_aec_cancels_delayed_echo_without_erasing_double_talk();
    test_aec_reports_residual_reference_correlation();
    test_aec_reports_residual_correlation_at_playback_delay();
    puts("aec tests: PASS");
    return 0;
}
