/* Pure audio-turn coordinator: no I/O, words, controls or generation allocation.
 * The sole core supplies tags only after its control decision. */
#ifndef LX06_DC_AUDIO_ROUTE_H
#define LX06_DC_AUDIO_ROUTE_H
#include "types.h"
#include <stddef.h>
#include <stdint.h>
#define DC_AUDIO_MIC_CAP 128000u
#define DC_AUDIO_PRE_BYTES 10240u
#define DC_AUDIO_TURNS 4u
enum dc_audio_action_type { DC_AUDIO_MIC=1,DC_AUDIO_COMMIT,DC_AUDIO_GRANT,DC_AUDIO_CANCEL };
enum dc_audio_begin_result { DC_AUDIO_BEGIN_STALE=0,DC_AUDIO_BEGIN_KNOWN=1,
                            DC_AUDIO_BEGIN_NEED_TAG=2,DC_AUDIO_BEGIN_CANCELLED=3 };
typedef struct dc_audio_action {
 unsigned type;dc_tag tag;uint64_t sequence,token;
 const unsigned char *pcm;size_t bytes;
} dc_audio_action;
typedef struct dc_audio_status {
 dc_tag cloud_tag,pending_tag;
 uint64_t cloud_sequence,highest_sequence,captured_bytes,sent_bytes;
 size_t microphone_bytes;unsigned turns,ready,protected_output,cancel_barrier;
 unsigned prefix_lost;
} dc_audio_status;
typedef struct dc_audio_route dc_audio_route;
dc_audio_route *dc_audio_route_create(uint64_t epoch,unsigned barge_any);
void dc_audio_route_destroy(dc_audio_route *);
int dc_audio_route_ready(dc_audio_route *);
/* Registers the core's independent initial cloud request, not a microphone
 * QUESTION. Its first BEGIN uses this tag, including after a pre-BEGIN cancel. */
int dc_audio_route_greeting(dc_audio_route *,dc_tag,uint64_t now_ms);
int dc_audio_route_local_greeting(dc_audio_route *,dc_tag,uint64_t now_ms);
void dc_audio_route_local_greeting_done(dc_audio_route *,dc_tag);
/* near_end is evidence from the core's playback-reference guard, not provider
 * VAD. utterance may initially be0 until the same recognizer exposes a serial. */
int dc_audio_route_capture(dc_audio_route *,const void *pcm,size_t bytes,
                           uint64_t utterance,int near_end,uint64_t now_ms);
/* Call after a genuine complete core ASR FINAL creates this question tag.
 * Freezes an upload watermark; later samples cannot extend this commit. */
int dc_audio_route_question(dc_audio_route *,dc_tag,uint64_t now_ms);
/* Call after dc_session_text processes every FINAL. Accepted question/control
 * keeps its audio; an ignored background utterance unpins protected retention. */
int dc_audio_route_final(dc_audio_route *,uint64_t utterance);
/* Called only after local STOP. Idempotent; an already committed pre-BEGIN
 * turn stays a tombstone and the next BEGIN is cancelled without a grant. */
int dc_audio_route_cancel(dc_audio_route *,dc_tag,uint64_t now_ms);
int dc_audio_route_begin(dc_audio_route *,uint64_t sequence,uint64_t now_ms,dc_tag *);
/* NEED_TAG permits only the core to allocate a native/initial reply tag. */
int dc_audio_route_bind(dc_audio_route *,uint64_t sequence,dc_tag,uint64_t now_ms);
/* End means cloud output finished. It does not unprotect the microphone until
 * the actual player drains (played), or cancellation teardown completes. */
int dc_audio_route_end(dc_audio_route *,uint64_t sequence,uint64_t generation);
int dc_audio_route_played(dc_audio_route *,dc_tag);
int dc_audio_route_resumed(dc_audio_route *,uint64_t sequence,uint64_t generation);
int dc_audio_route_accepts(const dc_audio_route *,uint64_t sequence,uint64_t generation);
/* next is a borrowed view. Send its header/body directly, then call sent only
 * after success. EAGAIN consumes nothing; any intervening mutation invalidates
 * the offered token, so re-query next instead of storing a PCM pointer. */
int dc_audio_route_next(dc_audio_route *,dc_audio_action *);
int dc_audio_route_sent(dc_audio_route *,const dc_audio_action *,uint64_t now_ms);
int dc_audio_route_tick(dc_audio_route *,uint64_t now_ms);
void dc_audio_route_status(const dc_audio_route *,dc_audio_status *);
#endif
