/* Processed-PNS PCM guard and manual gain. One reference writer (the player),
 * one microphone reader (the reactor). No hardware/cloud/session operations. */
#ifndef LX06_DC_VOICE_GUARD_H
#define LX06_DC_VOICE_GUARD_H
#include <stddef.h>
#include <stdint.h>
typedef struct dc_voice_guard dc_voice_guard;
typedef struct dc_voice_result {unsigned voiced,near_end,echo,clipped;double correlation;unsigned rms;} dc_voice_result;
dc_voice_guard *dc_voice_guard_create(void);
void dc_voice_guard_destroy(dc_voice_guard *);
void dc_voice_guard_reset_reference(dc_voice_guard *);
/* now_ms is the monotonic completion time of this accepted write, not a
 * per-sample playback/capture time. All samples from the write share it.
 * Correlation excludes stale/future samples and all windows crossing writes:
 * without a writer continuity contract even same-ms writes might contain gaps.
 * This fail-closed fallback can miss echo and cause false interruptions. */
void dc_voice_guard_reference(dc_voice_guard *,const int16_t *,size_t frames,unsigned rate,uint64_t now_ms);
/* Experimental seam, not wired to production. Nonzero session, stream
 * generation and operation plus accepted-frame offset must be assigned by a
 * trusted writer. Only adjacent offsets in the same identity may be joined.
 * These tokens do not establish physical playback or acoustic alignment. */
void dc_voice_guard_reference_span(dc_voice_guard *,const int16_t *,size_t frames,unsigned rate,uint64_t now_ms,
                                   uint64_t session,uint64_t generation,uint64_t operation,uint64_t offset);
/* Complete 10/20/30 ms, mono 16 kHz. Classification uses pre-gain PCM; gain
 * cannot turn a low-level noise frame into stronger evidence of human speech. */
int dc_voice_guard_process(dc_voice_guard *,const int16_t *,size_t frames,double gain_db,
                           int16_t *out,uint64_t now_ms,dc_voice_result *);
#endif
