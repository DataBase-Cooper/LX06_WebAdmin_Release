/* Provider-neutral local media. No player, ASR, thread, file or network.
 * All media shares the core's sole IO player and explicit tagged teardown. */
#ifndef LX06_DC_MEDIA_H
#define LX06_DC_MEDIA_H
#include "types.h"
#include <stddef.h>
#define DC_MEDIA_RATE 16000u
#define DC_MEDIA_FRAME_MAX 320u
#define DC_MEDIA_CUE_PREFILL_SAMPLES 960u /* 60 ms, strictly below 100 ms */
#define DC_MEDIA_CUE_GENERATION UINT64_MAX
enum dc_media_role { DC_MEDIA_CONNECTING=1, DC_MEDIA_GREETING,
    DC_MEDIA_REPLY, DC_MEDIA_ACTION_RESULT };
typedef struct dc_media_cue_config {
    unsigned frame_ms,on_ms,off_ms,frequency_a,frequency_b;
    double level_db; /* combined peak dBFS; gentle default -30 */
} dc_media_cue_config;
typedef struct dc_media_frame {
    dc_tag tag;
    enum dc_media_role role;
    unsigned rate,samples;
    int16_t pcm[DC_MEDIA_FRAME_MAX];
} dc_media_frame;
/* Caller-owned fixed storage; initialize to {0} before first begin. Oscillator
 * state is private. Stop before reusing this object for a new session. */
typedef struct dc_media_cue {
    dc_tag tag;
    dc_media_cue_config config;
    unsigned active,position,on_samples,cycle_samples;
    double amplitude,sine_a,cosine_a,sine_b,cosine_b;
    double step_sine_a,step_cosine_a,step_sine_b,step_cosine_b;
} dc_media_cue;
dc_media_cue_config dc_media_cue_defaults(void);
int dc_media_cue_begin(dc_media_cue *,uint64_t session_epoch,const dc_media_cue_config *);
/* Returns 1 frame, 0 stopped, or -errno. queued_samples is caller-observed
 * software PCM awaiting the shared player; -EAGAIN preserves oscillator/cycle
 * position. Never enqueue more than 60 ms. This does not claim a hardware
 * buffer bound: stop must call IO stop/drop with the returned reserved tag. */
int dc_media_cue_next(dc_media_cue *,size_t queued_samples,dc_media_frame *);
/* Invalidate immediately and return the old tag to stop the SAME IO player.
 * Repeated stop returns zero. READY/cancel/close all use this exact path. The
 * cue never consumes a cloud generation or changes CONNECTING to SPEAKING. */
dc_tag dc_media_cue_stop(dc_media_cue *);
int dc_media_cue_active(const dc_media_cue *);
#endif
