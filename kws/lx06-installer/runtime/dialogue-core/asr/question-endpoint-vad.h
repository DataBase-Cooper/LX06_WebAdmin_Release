/* Experimental QUESTION endpoint only. Never gate or modify uploaded audio.
 * Producer availability is the post-DNN publication time, NOT ADC capture.
 * The current raw-PCM pipe cannot provide it; callers without this metadata
 * must retain their original endpoint and may not invent read-time stamps.
 */
#ifndef LX06_QUESTION_ENDPOINT_VAD_H
#define LX06_QUESTION_ENDPOINT_VAD_H
#include <stddef.h>
#include <stdint.h>

enum qev_origin { QEV_LIVE=1, QEV_DEFERRED=2, QEV_REPLAY=3 };
enum qev_frame_code {
 QEV_FRAME_OK=0,QEV_NO_METADATA=1,QEV_NOT_LIVE=2,QEV_BAD_FORMAT=3,
 QEV_BAD_TIME=4,QEV_STALE=5,QEV_DISCONTINUITY=6,QEV_CLASSIFIER_ERROR=7
};
/* Each complete producer packet owns one sequence and availability time.
 * Do not reinterpret a large batch as multiple newly available packets. */
struct qev_stamp {
 uint64_t producer_birth_ticks,sequence,available_mono_ms;
 uint32_t producer_pid,rate,samples,origin;
};
struct qev_gate {
 uint64_t turn,last_partial_mono_ms;
 unsigned active,protect,partial_nonempty,ended,held,replaying;
 size_t pending_bytes,outbox_bytes,partial_pcm_bytes;
};
struct qev_stats {
 uint64_t turn,last_available_mono_ms,nonvoice_samples,nonvoice_span_ms;
 uint64_t real_vad_calls,valid_live_frames;
 unsigned voice_seen,frame_code,metadata_valid;
};
typedef struct qev_endpoint qev_endpoint;
/* profile 1 is QUESTION. Other profiles fail before allocation. */
qev_endpoint *qev_new(unsigned profile);
void qev_free(qev_endpoint *);
void qev_begin(qev_endpoint *,uint64_t turn,uint64_t turn_start_mono_ms);
/* Reads a complete immutable PCM packet. -1 rejects endpoint evidence only;
 * it never authorizes dropping the same packet from the ASR upload path. */
int qev_feed(qev_endpoint *,const int16_t *,size_t,const struct qev_stamp *,uint64_t read_mono_ms);
/* 1 means eligible to send E, not accepted text or an instruction to stop ASR.
 * A genuine complete cloud FINAL is still mandatory. Stable time is fixed1600ms. */
int qev_ready(const qev_endpoint *,const struct qev_gate *,uint64_t poll_mono_ms);
void qev_stats_read(const qev_endpoint *,struct qev_stats *);
#ifdef QEV_HOST_TEST
/* Test-only classifier injection exercises the actual endpoint state machine.
 * This symbol is absent from the non-test module. No speech accuracy claim. */
int qev_feed_decision_for_test(qev_endpoint *,size_t,const struct qev_stamp *,uint64_t,int);
#endif
#endif
