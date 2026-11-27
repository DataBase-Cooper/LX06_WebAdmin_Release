#ifndef WAKE_SAMPLE_H
#define WAKE_SAMPLE_H
#include "wake_cloud_verify.h"
#include <stddef.h>
/* Write recs/<word>/N.asr.json from an enrollment ASR result.
 * rc_text is the helper exit ("0","2","3","4") or a named clip status
 * ("too_short","too_long","unavailable"). asr_text may be empty.
 * Returns 0 when the sidecar is stored. The typed wake-word name is not written. */
int wake_sample_write(const char *json_path, const char *rc_text, const char *asr_text);
/* Fill word->sample_pinyin from N.asr.json next to N.wav. Files without a
 * usable pinyin increment missing and are not references. */
int wake_samples_load(const char *recdir, struct wake_cloud_word *word);
/* JSON array of wav-backed samples for web88. Always a complete array. */
int wake_samples_json(const char *recdir, char *out, size_t cap);
#endif
