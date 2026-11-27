#include "aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int16_t clamp_i16(float x) {
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

int xz_aec_init(xz_aec *aec, unsigned sample_rate, size_t taps,
                size_t delay_samples, float mu) {
    if (!aec || sample_rate == 0 || taps == 0 || taps > 4096 ||
        delay_samples > sample_rate || mu <= 0.0f || mu > 1.0f) return -1;
    memset(aec, 0, sizeof(*aec));
    aec->history_size = delay_samples + taps + 1;
    aec->weights = (float *)calloc(taps, sizeof(*aec->weights));
    aec->history = (int16_t *)calloc(aec->history_size, sizeof(*aec->history));
    aec->tap_cross_energy = (double *)calloc(taps, sizeof(*aec->tap_cross_energy));
    aec->tap_reference_energy = (double *)calloc(taps, sizeof(*aec->tap_reference_energy));
    if (!aec->weights || !aec->history || !aec->tap_cross_energy ||
        !aec->tap_reference_energy) {
        xz_aec_destroy(aec);
        return -1;
    }
    aec->sample_rate = sample_rate;
    aec->taps = taps;
    aec->delay_samples = delay_samples;
    aec->mu = mu;
    return 0;
}

void xz_aec_destroy(xz_aec *aec) {
    if (!aec) return;
    free(aec->weights);
    free(aec->history);
    free(aec->tap_cross_energy);
    free(aec->tap_reference_energy);
    memset(aec, 0, sizeof(*aec));
}

int xz_aec_process(xz_aec *aec, const int16_t *reference,
                   const int16_t *microphone, int16_t *residual, size_t samples) {
    if (!aec || !aec->weights || !aec->history || !reference || !microphone ||
        !residual || samples == 0) return -1;

    memset(aec->tap_cross_energy, 0, aec->taps * sizeof(*aec->tap_cross_energy));
    memset(aec->tap_reference_energy, 0,
           aec->taps * sizeof(*aec->tap_reference_energy));
    double residual_energy = 0.0;
    for (size_t n = 0; n < samples; ++n) {
        aec->history[aec->write_index] = reference[n];
        float estimate = 0.0f;
        float norm = 1.0e6f;
        for (size_t k = 0; k < aec->taps; ++k) {
            size_t lag = aec->delay_samples + k;
            size_t idx = (aec->write_index + aec->history_size - lag) % aec->history_size;
            float x = (float)aec->history[idx];
            estimate += aec->weights[k] * x;
            norm += x * x;
        }

        float error = (float)microphone[n] - estimate;
        residual[n] = clamp_i16(error);
        double err = (double)residual[n];
        residual_energy += err * err;

        /* Estimate whether the remaining signal still follows any playback tap,
           not just the same-sample reference. Acoustic paths are delayed. */
        float scale = aec->mu * error / norm;
        for (size_t k = 0; k < aec->taps; ++k) {
            size_t lag = aec->delay_samples + k;
            size_t idx = (aec->write_index + aec->history_size - lag) % aec->history_size;
            double x = (double)aec->history[idx];
            aec->tap_cross_energy[k] += x * err;
            aec->tap_reference_energy[k] += x * x;
            aec->weights[k] += scale * (float)x;
        }
        aec->write_index = (aec->write_index + 1) % aec->history_size;
    }

    float max_correlation = 0.0f;
    for (size_t k = 0; k < aec->taps; ++k) {
        double denominator = sqrt(aec->tap_reference_energy[k] * residual_energy);
        if (denominator <= 0.0) continue;
        float correlation = (float)(aec->tap_cross_energy[k] / denominator);
        if (correlation < 0.0f) correlation = -correlation;
        if (correlation > max_correlation) max_correlation = correlation;
    }
    aec->last_correlation = max_correlation > 1.0f ? 1.0f : max_correlation;
    return 0;
}
