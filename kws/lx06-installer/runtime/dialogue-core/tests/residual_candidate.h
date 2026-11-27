/* Offline experiment only. timestamp is the reference block's monotonic time. */
#ifndef RESIDUAL_CANDIDATE_H
#define RESIDUAL_CANDIDATE_H
#include <stdint.h>
#include <stddef.h>
typedef struct residual_candidate residual_candidate;
residual_candidate *residual_candidate_new(void);
void residual_candidate_free(residual_candidate *c);
int residual_candidate_process(residual_candidate *c,const int16_t *mic,const int16_t *residual,size_t n,uint64_t reference_ms,int *trigger);
#endif
