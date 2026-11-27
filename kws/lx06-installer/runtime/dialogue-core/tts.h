/* Direct, nonblocking hot-broker TTS for the unified core. The module owns no
 * player, ASR, microphone or light. Two URL synthesis slots, one decoder, fixed
 * PCM ring; signed URLs cross only a private pipe and never argv/logs/files. */
#ifndef LX06_DC_TTS_H
#define LX06_DC_TTS_H
#include "types.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DC_TTS_SLOTS 2u
#define DC_TTS_PCM_CAP 32768u
#define DC_TTS_PCM_VIEW_MAX 4096u

typedef struct dc_tts dc_tts;
typedef struct dc_tts_config {
    const char *socket_path;
    const char *decoder_path; /* exact nominated native-url-audio executable */
    pid_t broker_pid; /* optional >0 exact SO_PEERCRED pin; uid always checked */
} dc_tts_config;
enum dc_tts_event_type {
    DC_TTS_URL_READY = 1, DC_TTS_SEGMENT_DONE, DC_TTS_ERROR, DC_TTS_WARM_READY
};
enum dc_tts_error {
    DC_TTS_ERR_IO=1, DC_TTS_ERR_PROTOCOL, DC_TTS_ERR_BROKER,
    DC_TTS_ERR_DECODER, DC_TTS_ERR_TIMEOUT, DC_TTS_ERR_LIMIT
};
typedef struct dc_tts_event {
    enum dc_tts_event_type type;
    dc_tag tag;
    uint64_t segment_sequence, at_ms;
    enum dc_tts_error error;
    unsigned rate;
} dc_tts_event;
typedef struct dc_tts_pcm_view {
    dc_tag tag;
    uint64_t segment_sequence;
    unsigned rate;
    const unsigned char *data; /* borrowed until next poll/consume/cancel */
    size_t bytes; /* even, <=4096, mono S16LE */
} dc_tts_pcm_view;

dc_tts *dc_tts_create(const dc_tts_config *);
int dc_tts_destroy(dc_tts *); /* -1/EBUSY until shutdown is quiescent */
/* Session teardown is independent of a reply tag: it also closes a generation
 * zero warm with no greeting/request. Do not emit END until quiescent is true. */
void dc_tts_shutdown(dc_tts *, uint64_t now_ms);
int dc_tts_quiescent(const dc_tts *);
int dc_tts_warm(dc_tts *, dc_tag, uint64_t now_ms);
int dc_tts_submit(dc_tts *, dc_tag, uint64_t sequence, const char *text, uint64_t now_ms);
/* Synthesis URL may be prefetched. Only the current oldest sequence activates
 * a decoder. Activate first segment immediately; for following segments wait
 * until the old decoder is done and IO player queue is below about 100 ms.
 * Existing decoder's 1s stdout timeout is not silently changed. */
int dc_tts_activate(dc_tts *, dc_tag, uint64_t sequence, uint64_t now_ms);
int dc_tts_poll(dc_tts *, uint64_t now_ms, dc_tts_event *); /* 1 event,0,-1 */
/* Pull PCM only after successful IO player write: peek, write(view), consume.
 * This preserves bytes on EAGAIN without another copy/queue in the core. */
int dc_tts_peek_pcm(dc_tts *, dc_tts_pcm_view *);
int dc_tts_consume_pcm(dc_tts *, dc_tag, uint64_t sequence, size_t bytes);
/* Returns oldest outstanding sequence (1) with ready flag; 0 if no slots. */
int dc_tts_next(const dc_tts *, dc_tag *, uint64_t *sequence, int *url_ready);
unsigned dc_tts_slots(const dc_tts *);
int dc_tts_decoder_active(const dc_tts *);
int dc_tts_cancel(dc_tts *, dc_tag, uint64_t now_ms);
void dc_tts_tick(dc_tts *, uint64_t now_ms);
unsigned dc_tts_retired(const dc_tts *);
size_t dc_tts_storage_bytes(void);
#endif
