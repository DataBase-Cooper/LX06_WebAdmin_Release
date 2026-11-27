#ifndef XZ_AEC_H
#define XZ_AEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned sample_rate;
    size_t taps;
    size_t delay_samples;
    size_t history_size;
    size_t write_index;
    float mu;
    float last_correlation;
    float *weights;
    int16_t *history;
    double *tap_cross_energy;
    double *tap_reference_energy;
} xz_aec;

int xz_aec_init(xz_aec *aec, unsigned sample_rate, size_t taps,
                size_t delay_samples, float mu);
void xz_aec_destroy(xz_aec *aec);
int xz_aec_process(xz_aec *aec, const int16_t *reference,
                   const int16_t *microphone, int16_t *residual, size_t samples);

#endif
