#define _POSIX_C_SOURCE 200809L
/* LX06 Xiaozhi session orchestration. Authentication/configuration is supplied
 * by the user's private deployment; it does not belong in the source archive.
 * XZ_BARGE_ANY selects native speech interruption or Xiaomi control-word ASR.
 * In the explicit dual modes a goodbye exits once; an interruption aborts one
 * reply and resumes listening on the same connection. Reject delayed ASR from
 * older reply tags and residual TTS frames until the next genuine TTS start,
 * otherwise a second listen-start can discard the user's immediate follow-up.
 */
#include "aec.h"
#include "input_gain.h"
#include "event.h"
#include "home_mcp.h"
#include "../../../lx06-installer/runtime/doubao/src/echo.h"
#include "identity.h"
#include "json.h"
#include "opus_codec.h"
#include "player.h"
#include "../../../lx06-installer/hooks/xiaoai/dialog-light.h"
#include "../../../lx06-installer/runtime/common/control_asr.h"
#include "protocol.h"
#include "session.h"
#include "text_match.h"
#include "talk.h"
#include "task_poll.h"
#include "timeline.h"
#include "ws.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define XZ_MIC_RATE 16000
#define XZ_FRAME_MS 60
#define XZ_MIC_SAMPLES (XZ_MIC_RATE * XZ_FRAME_MS / 1000)
#define XZ_MIC_BYTES (XZ_MIC_SAMPLES * sizeof(int16_t))
#define XZ_OPUS_PACKET_CAP 4000
#define XZ_MAX_SERVER_SAMPLES 2880
#define XZ_REFERENCE_RATE 16000
#define XZ_REFERENCE_SECONDS 8
#define XZ_REFERENCE_CAPACITY (XZ_REFERENCE_RATE * XZ_REFERENCE_SECONDS)
#define XZ_MESSAGE_CAP XZ_WS_MAX_MESSAGE
#define XZ_MANUAL_WORD_QUIET_MS 180u
#define XZ_MANUAL_WORD_GAP_MS 300u
#define XZ_MANUAL_TURN_TAG (UINT64_C(1) << 63)
#define XZ_MANUAL_HOLD_FRAMES 36u
#define XZ_KEEPALIVE_INTERVAL_MS 25000u
#define XZ_TASK_QUIET_MS 3000u
#define XZ_BARGE_HISTORY 36u /* 2.16 seconds of filtered PCM */
#define XZ_BARGE_DEFAULT_RMS 1400u

static volatile sig_atomic_t g_stop_requested;
static const char *g_listen_mode = "auto";
static unsigned g_listen_pause_ms;
static unsigned g_manual_speech_frames;
static unsigned g_manual_quiet_ms;
static const char *g_manual_end_word = "";
static uint64_t g_manual_turn;
static int g_manual_end_confirmed;
typedef struct {
    size_t size;
    unsigned char data[XZ_OPUS_PACKET_CAP];
} xz_held_packet;
typedef struct {
    uint64_t first, next, cue_at;
    xz_held_packet packets[XZ_MANUAL_HOLD_FRAMES];
} xz_manual_audio;
static xz_manual_audio *g_manual_audio;
static const char *g_interrupt_list = "";
static const char *g_open_list = "";
static int g_phrase_uplink = 0;
static int g_keyword_mode, g_human_mode, g_waiting, g_interrupt_consumed;
static uint64_t g_reply_serial=1;
static int g_discard_reply_tail;

static void on_signal(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

/* keeper 受限作业的 RLIMIT_FSIZE（64KB）被写超限时，内核给写进程发 SIGXFSZ
   （默认动作是终止）。这里把时间线日志截断为 0，下一次 O_APPEND 写自动
   从 0 重新开始，长会话不因日志超限中途死掉。处理程序内只做 ftruncate
   （async-signal-safe），不做任何 I/O 格式化。 */
static int g_xfsz_fd = -1;
static int g_talk_fd = -1;
static xz_talk *g_talk = NULL;
static void on_xfsz(int signal_number) {
    (void)signal_number;
    if (g_xfsz_fd >= 0) (void)ftruncate(g_xfsz_fd, 0);
    if (g_talk_fd >= 0) (void)ftruncate(g_talk_fd, 0);
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned milliseconds) {
    struct timespec delay = {
        (time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static const char *env_or(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static int env_enabled(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
}

static long env_long(const char *name, long fallback, long minimum, long maximum) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum) return fallback;
    return parsed;
}

static void emit(xz_timeline *timeline, const char *event, const char *format, ...) {
    char details[320] = "";
    if (format && *format) {
        va_list args;
        va_start(args, format);
        int written=vsnprintf(details, sizeof(details), format, args);
        if(written>=(int)sizeof(details)) {
            size_t end=strlen(details),lead=end;
            while(lead && ((unsigned char)details[lead-1]&0xc0)==0x80)lead--;
            if(lead) {
                lead--;
                unsigned char c=(unsigned char)details[lead];
                size_t width=c>=0xf0?4:c>=0xe0?3:c>=0xc0?2:1;
                if(end-lead<width)details[lead]=0;
            }
        }
        va_end(args);
    }
    (void)xz_timeline_emit(timeline, event, details);
}

static FILE *open_timeline(void) {
    const char *path = getenv("XZ_TIMELINE_FILE");
    if (!path || !*path || strcmp(path, "-") == 0 || strcmp(path, "stderr") == 0)
        return stderr;
    if (strcmp(path, "stdout") == 0) return stdout;
    FILE *stream = fopen(path, "a+");
    if (!stream)
        /* 受限 uid 下日志文件可能缺失或不可写：不能因为打不开日志就杀会话，
           降级到 /dev/null 继续运行。 */
        stream = fopen("/dev/null", "a");
    if (stream) setvbuf(stream, NULL, _IOLBF, 0);
    return stream;
}

static int read_device_id(char *out, size_t capacity) {
    if (!out || capacity < 2) return -1;
    const char *configured = getenv("XZ_DEVICE_ID");
    if (configured && *configured) {
        if (strlen(configured) >= capacity) return -1;
        strcpy(out, configured);
        return 0;
    }
    FILE *file = fopen("/sys/class/net/wlan0/address", "r");
    if (!file) return -1;
    int ok = fgets(out, (int)capacity, file) != NULL;
    fclose(file);
    if (!ok) return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int open_microphone(const char *path, int *is_fifo) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    errno = 0;
    *is_fifo = lseek(fd, 0, SEEK_CUR) < 0 && errno == ESPIPE;
    return fd;
}

static int wait_for_microphone(const char *path, int timeout_ms, int *is_fifo) {
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    do {
        int fd = open_microphone(path, is_fifo);
        if (fd >= 0) return fd;
        sleep_ms(50);
    } while (!g_stop_requested && monotonic_ms() < deadline);
    return -1;
}

static int send_text(xz_ws *ws, xz_timeline *timeline, const char *event,
                     const char *json) {
    if (xz_ws_send_text(ws, json) != 0) {
        emit(timeline, "WS_SEND_ERROR", "kind=%s", event);
        return -1;
    }
    emit(timeline, event, "bytes=%zu", strlen(json));
    return 0;
}

static int append_spoken_text(char *target, size_t capacity, const char *text) {
    size_t used = strlen(target), incoming = strlen(text);
    if (incoming == 0) return 0;
    if (used && used + 1 < capacity) target[used++] = ' ';
    size_t copy = capacity - used - 1;
    if (incoming > copy) incoming = copy;
    memcpy(target + used, text, incoming);
    target[used + incoming] = '\0';
    return 0;
}

typedef struct {
    int16_t samples[XZ_REFERENCE_CAPACITY];
    uint64_t total_samples;
    uint64_t start_mono_ms;
    int started;
} xz_reference_ring;

static void reference_append(xz_reference_ring *ring, const int16_t *pcm,
                             size_t samples, int sample_rate) {
    if (!ring || !pcm || samples == 0 || (sample_rate != 16000 && sample_rate != 24000)) return;
    if (!ring->started) {
        ring->started = 1;
        ring->start_mono_ms = monotonic_ms();
        ring->total_samples = 0;
    }
    size_t output_samples = (size_t)((uint64_t)samples * XZ_REFERENCE_RATE /
                                     (unsigned)sample_rate);
    for (size_t i = 0; i < output_samples; ++i) {
        double source_position = (double)i * sample_rate / XZ_REFERENCE_RATE;
        size_t left = (size_t)source_position;
        size_t right = left + 1 < samples ? left + 1 : left;
        double fraction = source_position - left;
        double value = pcm[left] * (1.0 - fraction) + pcm[right] * fraction;
        ring->samples[ring->total_samples % XZ_REFERENCE_CAPACITY] = (int16_t)value;
        ring->total_samples++;
    }
}

static void reference_frame(const xz_reference_ring *ring, uint64_t capture_start_ms,
                            unsigned output_delay_ms, int16_t *out) {
    memset(out, 0, XZ_MIC_BYTES);
    if (!ring || !ring->started || capture_start_ms < ring->start_mono_ms + output_delay_ms)
        return;
    uint64_t offset_ms = capture_start_ms - ring->start_mono_ms - output_delay_ms;
    uint64_t first_sample = offset_ms * XZ_REFERENCE_RATE / 1000u;
    uint64_t oldest = ring->total_samples > XZ_REFERENCE_CAPACITY ?
                      ring->total_samples - XZ_REFERENCE_CAPACITY : 0;
    for (size_t i = 0; i < XZ_MIC_SAMPLES; ++i) {
        uint64_t index = first_sample + i;
        if (index >= oldest && index < ring->total_samples)
            out[i] = ring->samples[index % XZ_REFERENCE_CAPACITY];
    }
}

static unsigned frame_rms(const int16_t *pcm) {
    long double energy = 0.0;
    for (size_t i = 0; i < XZ_MIC_SAMPLES; ++i)
        energy += (long double)pcm[i] * pcm[i];
    return (unsigned)sqrt((double)(energy / XZ_MIC_SAMPLES));
}

static int frame_speech_like(const int16_t *pcm, unsigned rms) {
    if (rms < 250) return 0;
    unsigned crossings = 0;
    for (size_t i = 1; i < XZ_MIC_SAMPLES; ++i)
        if ((pcm[i - 1] < 0 && pcm[i] >= 0) || (pcm[i - 1] >= 0 && pcm[i] < 0))
            crossings++;
    double rate = (double)crossings / XZ_MIC_SAMPLES;
    return rate >= 0.01 && rate <= 0.30;
}

typedef struct {
    int16_t pcm[XZ_BARGE_HISTORY][XZ_MIC_SAMPLES];
    unsigned next, used;
    uint64_t last_voice, partial_since, guard_tail_until;
    char partial[2048];
} xz_human_barge;

static void observe_playback(void *context, const unsigned char *pcm, size_t bytes,
                             unsigned rate) {
    db_echo_reference_rate(context, pcm, bytes, monotonic_ms(), rate);
}

static unsigned char pending_playback[XZ_PLAYER_QUEUE_CAPACITY];
static size_t pending_playback_bytes;

static int queue_pending_playback(const void *pcm, size_t bytes) {
    if (bytes > sizeof(pending_playback) - pending_playback_bytes) return -1;
    memcpy(pending_playback + pending_playback_bytes, pcm, bytes);
    pending_playback_bytes += bytes;
    return 0;
}

static int start_player(xz_player *player, int *ready, const char *program,
                        const char *device, int sample_rate, db_echo *echo) {
    if (!*ready) {
        if (xz_player_init(player) != 0) return -1;
        *ready = 1;
    } else if (player->done || !player->thread_started) {
        xz_player_destroy(player);
        if (xz_player_init(player) != 0) {
            *ready = 0;
            return -1;
        }
    }
    if (player->thread_started) return 0;
    player->observe_pcm = echo ? observe_playback : NULL;
    player->observe_context = echo;
    return xz_player_start(player, program, device, sample_rate);
}

static void abort_player(xz_player *player, int *ready) {
    pending_playback_bytes = 0;
    if (!*ready) return;
    (void)xz_player_abort(player);
    xz_player_destroy(player);
    *ready = 0;
}

static void finish_player(xz_player *player, int *ready, xz_timeline *timeline,
                          unsigned timeout_ms) {
    if (!*ready) return;
    if (player->thread_started && !player->done) (void)xz_player_finish(player);
    uint64_t deadline = monotonic_ms() + timeout_ms;
    int exit_code = -1;
    while (player->thread_started && xz_player_poll(player, &exit_code) == 0 &&
           monotonic_ms() < deadline) sleep_ms(10);
    if (player->thread_started && xz_player_poll(player, &exit_code) == 0) {
        emit(timeline, "PLAYBACK_DRAIN_TIMEOUT", "timeout_ms=%u", timeout_ms);
        abort_player(player, ready);
        return;
    }
    if (player->thread_started) {
        emit(timeline, "PLAYBACK_EXIT", "exit=%d", exit_code);
        xz_player_destroy(player);
    } else {
        xz_player_destroy(player);
    }
    *ready = 0;
}

static int send_listen_start(xz_ws *ws, xz_timeline *timeline,
                             const char *session_id) {
    char json[256];
    if (xz_protocol_build_listen(json, sizeof(json), session_id, "start",
                                 g_listen_mode) != 0)
        return -1;
    if (g_listen_pause_ms) {
        g_waiting = 0;
        g_manual_speech_frames = g_manual_quiet_ms = 0;
        g_manual_end_confirmed = 0;
        ++g_manual_turn;
        if (g_manual_audio) {
            g_manual_audio->first = g_manual_audio->next = 0;
            g_manual_audio->cue_at = UINT64_MAX;
        }
    }
    return send_text(ws, timeline, "LISTEN_START", json);
}

static int send_task_query(xz_ws *ws, xz_timeline *timeline, const char *session_id,
                           xz_task_poll *poll, xz_task *task, const char *fallback, uint64_t now) {
    char query[XZ_TASK_QUERY_MAX + XZ_TASK_ID_MAX + 1], json[768];
    task->query_at = now + XZ_TASK_POLL_MS;
    const char *pattern = *task->query ? task->query : fallback;
    if (xz_task_query_text(query, sizeof(query), pattern, task->id) != 0 ||
        xz_protocol_build_detect(json, sizeof(json), session_id, query) != 0) {
        emit(timeline, "MCP_TASK_QUERY_INVALID", "");
        return 0;
    }
    if (send_text(ws, timeline, "MCP_TASK_QUERY", json) != 0) return -1;
    poll->awaiting_until = now + XZ_TASK_REPLY_MS;
    g_waiting = 1;
    ++g_reply_serial;
    return 0;
}

static int cancel_task_query(xz_ws *ws, xz_timeline *timeline, const char *session_id,
                             xz_task_poll *poll, const char *event) {
    char json[256];
    if (xz_protocol_build_abort(json, sizeof(json), session_id, "wake_word_detected") != 0 ||
        send_text(ws, timeline, event, json) != 0)
        return -1;
    poll->awaiting_until = 0;
    g_waiting = 0;
    g_discard_reply_tail = 1;
    return send_listen_start(ws, timeline, session_id);
}

static int send_held_audio(xz_ws *ws, uint64_t end) {
    xz_manual_audio *audio = g_manual_audio;
    if (!audio) return 0;
    while (audio->first < end) {
        xz_held_packet *packet = &audio->packets[audio->first % XZ_MANUAL_HOLD_FRAMES];
        if (xz_ws_send_binary(ws, packet->data, packet->size) != 0) return -1;
        ++audio->first;
    }
    return 0;
}

static int stage_manual_audio(xz_ws *ws, const unsigned char *packet, size_t size) {
    xz_manual_audio *audio = g_manual_audio;
    if (!audio) return xz_ws_send_binary(ws, packet, size);
    if (audio->next - audio->first == XZ_MANUAL_HOLD_FRAMES &&
        send_held_audio(ws, audio->first + 1) != 0)
        return -1;
    xz_held_packet *saved = &audio->packets[audio->next++ % XZ_MANUAL_HOLD_FRAMES];
    memcpy(saved->data, packet, size);
    saved->size = size;
    return 0;
}

static int replay_barge_audio(xz_human_barge *barge, xz_ws *ws,
                              xz_opus_codec *codec) {
    while (barge->used) {
        unsigned index = (barge->next + XZ_BARGE_HISTORY - barge->used) % XZ_BARGE_HISTORY;
        const int16_t *pcm = barge->pcm[index];
        unsigned char packet[XZ_OPUS_PACKET_CAP];
        int bytes = xz_opus_encode(codec, pcm, XZ_MIC_SAMPLES, packet, sizeof(packet));
        if (bytes <= 0 || stage_manual_audio(ws, packet, (size_t)bytes)) return -1;
        if (g_listen_pause_ms && frame_speech_like(pcm, frame_rms(pcm))) {
            if (g_manual_speech_frames < 2) ++g_manual_speech_frames;
            g_manual_quiet_ms = 0;
        }
        --barge->used;
    }
    return 0;
}

static int manual_cue_is_held(void) {
    return g_manual_audio && g_manual_audio->cue_at >= g_manual_audio->first &&
           g_manual_audio->cue_at < g_manual_audio->next;
}

static int send_manual_stop(xz_ws *ws, xz_timeline *timeline, const char *session_id,
                            int cue) {
    if (g_manual_audio) {
        /* A confirmed standalone marker has never left the local holdback.
           Flush the complete preceding request and discard marker audio. */
        if (send_held_audio(ws, cue ? g_manual_audio->cue_at : g_manual_audio->next) != 0)
            return -1;
        g_manual_audio->first = g_manual_audio->next;
        g_manual_audio->cue_at = UINT64_MAX;
    }
    char json[256];
    if (xz_protocol_build_listen(json, sizeof(json), session_id, "stop", NULL) != 0 ||
        send_text(ws, timeline, "LISTEN_STOP", json) != 0)
        return -1;
    g_waiting = 1;
    g_reply_serial++;
    (void)lx06_dialog_light(LX06_LIGHT_THINKING);
    emit(timeline, "MANUAL_TURN_ENDED", "reason=%s quiet_ms=%u configured_ms=%u",
         cue ? "end_word" : "silence", g_manual_quiet_ms, g_listen_pause_ms);
    return 0;
}

static int finish_manual_turn(xz_ws *ws, xz_timeline *timeline,
                              const char *session_id, int speech_like) {
    if (speech_like) {
        if (g_manual_speech_frames < 2) g_manual_speech_frames++;
        g_manual_quiet_ms = 0;
        return 0;
    }
    if (g_manual_speech_frames < 2) {
        g_manual_speech_frames = 0;
        return 0;
    }
    g_manual_quiet_ms += XZ_FRAME_MS;
    if (g_manual_end_confirmed && manual_cue_is_held() && g_manual_quiet_ms >= XZ_MANUAL_WORD_QUIET_MS)
        return send_manual_stop(ws, timeline, session_id, 1);
    if (g_manual_quiet_ms >= g_listen_pause_ms)
        return send_manual_stop(ws, timeline, session_id, 0);
    return 0;
}

/* The server clears the previous VAD/ASR buffers only when the device sends
 * listen/start.  Re-arm only after local output has completely drained so the
 * next turn cannot inherit either stale ASR state or output-tail audio.  When
 * the user's last utterance matched the configured goodbye phrase the re-arm
 * is replaced by a protocol goodbye: the session is ending, not continuing. */
typedef struct {
    const char *phrase;
    int pending;
    uint64_t seen_ms;
    uint64_t sent_ms;
} xz_goodbye_state;

static int send_goodbye(xz_ws *ws, xz_session *session, xz_timeline *timeline,
                        xz_goodbye_state *goodbye) {
    char json[256];
    if (xz_protocol_build_goodbye(json, sizeof(json), session->session_id) != 0)
        return -1;
    if (send_text(ws, timeline, "GOODBYE_SENT", json) != 0) return -1;
    (void)lx06_dialog_light(LX06_LIGHT_END);
    goodbye->pending = 0;
    goodbye->sent_ms = monotonic_ms();
    return 0;
}

static int resume_after_reply(xz_ws *ws, xz_session *session, xz_timeline *timeline,
                              int *tts_active, uint64_t *echo_until_ms,
                              const char *listen_mode, xz_goodbye_state *goodbye) {
    /* An old player can finish after the next cloud TTS has already started.
     * Its drain must not clear the new utterance or send a stale listen/start. */
    if (*tts_active || pending_playback_bytes) return 0;
    g_waiting=0;
    *echo_until_ms = monotonic_ms() + 1200;
    if (goodbye->pending) {
        if (send_goodbye(ws, session, timeline, goodbye) != 0)
            return -1;
        return 0;
    }
    if (goodbye->sent_ms) {
        /* The protocol goodbye is already on the wire: the session is
         * ending.  A farewell TTS from the server may still arrive; let it
         * drain but do not re-arm LISTEN_START on a closing server. */
        return 0;
    }
    /* Realtime stays armed during TTS. Sending listen/start here clears the
     * utterance whose VAD just stopped server playback, before STT can arrive. */
    (void)lx06_dialog_light(LX06_LIGHT_LISTENING);
    if (!strcmp(listen_mode, "realtime")) {
        emit(timeline, "LISTEN_CONTINUE", "mode=realtime");
        return 0;
    }
    if (send_listen_start(ws, timeline, session->session_id) != 0) {
        emit(timeline, "LISTEN_RESTART_SEND_ERROR", "");
        return -1;
    }
    emit(timeline, "LISTEN_RESUME", "mode=%s", listen_mode);
    return 0;
}

static int handle_server_text(xz_ws *ws, xz_session *session, xz_timeline *timeline,
                              const unsigned char *data, size_t length,
                              int *tts_active, char *spoken_text, size_t spoken_cap,
                              uint64_t *echo_until_ms, int *pending_barge,
                              int barge_enabled, xz_barge_gate *barge_gate,
                              xz_player *player, int *player_ready, int *player_active,
                              int *playback_draining, uint64_t *playback_tail_until_ms,
                              const char *listen_mode, xz_goodbye_state *goodbye,
                              uint64_t *last_activity, xz_home_mcp *home) {
    xz_server_event event;
    if (xz_event_parse_text((const char *)data, length, &event) != 0) {
        emit(timeline, "SERVER_JSON_ERROR", "bytes=%zu", length);
        return 0;
    }

    switch (event.type) {
        case XZ_EVENT_TTS:
            if (strcmp(event.state, "start") == 0) {
                g_discard_reply_tail=0;
                if (xz_session_tts_start(session) != 0) {
                    emit(timeline, "STATE_ERROR", "event=TTS_START");
                    return -1;
                }
                *tts_active = 1;
                /* A new reply must not inherit the previous reply's tail. */
                *playback_tail_until_ms = 0;
                if(!g_waiting)g_reply_serial++;
                g_waiting=0;g_interrupt_consumed=0;
                if(!*player_active && !*playback_draining)
                    (void)lx06_dialog_light(LX06_LIGHT_THINKING);
                *last_activity = monotonic_ms();
                *pending_barge = 0;
                spoken_text[0] = '\0';
                xz_barge_gate_reset(barge_gate);
                if (event.text[0])
                    append_spoken_text(spoken_text, spoken_cap, event.text);
                emit(timeline, "TTS_START", "text_bytes=%zu", event.text_bytes);
            } else if (strcmp(event.state, "sentence_start") == 0) {
                append_spoken_text(spoken_text, spoken_cap, event.text);
                emit(timeline, "TTS_SENTENCE_START", "text_bytes=%zu", event.text_bytes);
            } else if (strcmp(event.state, "stop") == 0) {
                if(g_discard_reply_tail) {
                    emit(timeline,"CANCELED_TTS_STOP_IGNORED","listening_unchanged");break;
                }
                if (*tts_active && xz_session_tts_stop(session) != 0) {
                    emit(timeline, "STATE_ERROR", "event=TTS_STOP");
                    return -1;
                }
                *pending_barge = 0;
                xz_barge_gate_reset(barge_gate);
                if (spoken_text[0]) xz_talk_log(g_talk, "xiaozhi", spoken_text);
                emit(timeline, "TTS_STOP", "text_bytes=%zu", event.text_bytes);
                /* TTS has ended at the protocol layer.  Keep the microphone
                 * muted with playback_draining, but mark this false now so a
                 * server close drains the final queued reply rather than
                 * aborting its player in the session cleanup path. */
                *tts_active = 0;
                if (*player_active) {
                    /* Keep draining and muting microphone FIFO data until the
                     * local player has truly released the output device.  A
                     * synchronous wait here used to let output-tail PCM pile
                     * up, then upload it as the next user utterance. */
                    if (xz_player_finish(player) != 0) {
                        emit(timeline, "PLAYBACK_FINISH_ERROR", "");
                        abort_player(player, player_ready);
                        *player_active = 0;
                    } else {
                        *playback_draining = 1;
                        *playback_tail_until_ms = 0;
                        break;
                    }
                }
                if (resume_after_reply(ws, session, timeline, tts_active,
                                       echo_until_ms, listen_mode, goodbye) != 0)
                    return -1;
            }
            break;
        case XZ_EVENT_STT: {
            if(g_keyword_mode && (xz_text_matches_list(event.text,g_interrupt_list) ||
                                  xz_text_matches_list(event.text,goodbye->phrase))) {
                emit(timeline,"PRIMARY_CONTROL_IGNORED","speaker_asr_owns_controls");
                break;
            }
            uint64_t now = monotonic_ms();
            int speaking = *tts_active || *playback_draining ||
                           (player_active && *player_active);
            if (g_human_mode && speaking) break;
            /* 自己刚说的话里含开启/结束/打断词：回声命中不算用户。
             * 对照 outbound 文本，不看音量。豆包端到端复用 xz_self_echo。 */
            if (event.text[0] && spoken_text[0] &&
                (speaking || now <= *echo_until_ms + 3000) &&
                xz_self_echo(event.text, spoken_text, g_open_list,
                             goodbye->phrase, g_interrupt_list)) {
                emit(timeline, "SELF_ECHO_SUPPRESSED", "text_bytes=%zu", event.text_bytes);
                *pending_barge = 0;
                break;
            }
            /* Realtime cloud VAD may stop TTS before final ASR arrives. A
             * configured control phrase still cancels the pending reply and
             * keeps this conversation open, including while already listening. */
            if (!g_keyword_mode && !g_human_mode && xz_text_matches_list(event.text, g_interrupt_list) &&
                !xz_text_matches_list(event.text, goodbye->phrase)) {
                if(g_interrupt_consumed) {
                    emit(timeline,"REPEATED_INTERRUPT_IGNORED","listening_unchanged");break;
                }
                g_interrupt_consumed=1;
                g_discard_reply_tail=1;
                char abort_json[256];
                emit(timeline, "INTERRUPT_WORD", "text_bytes=%zu", event.text_bytes);
                if (xz_protocol_build_abort(abort_json, sizeof(abort_json),
                                            session->session_id,
                                            "wake_word_detected") != 0 ||
                    xz_ws_send_text(ws, abort_json) != 0) {
                    emit(timeline, "INTERRUPT_ABORT_SEND_ERROR", "");
                    return -1;
                }
                abort_player(player, player_ready);
                *player_active = 0;
                if (session->state == XZ_SPEAKING && xz_session_abort(session) != 0) return -1;
                *tts_active = 0;
                *playback_draining = 0;
                *pending_barge = 0;
                xz_barge_gate_reset(barge_gate);
                emit(timeline, "ABORT_SENT", "reason=interrupt_word");
                g_waiting=0;
                (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                if (send_listen_start(ws, timeline, session->session_id) != 0)
                    return -1;
                break;
            }
            if (event.text[0] && now <= *echo_until_ms &&
                xz_text_is_echo(event.text, spoken_text)) {
                emit(timeline, "STT_ECHO_SUPPRESSED", "text_bytes=%zu", event.text_bytes);
                *pending_barge = 0;
                break;
            }
            emit(timeline, "STT_RECOGNIZED", "text_bytes=%zu", event.text_bytes);
            if(event.text[0]) g_interrupt_consumed=0;
            if(event.text[0] && !speaking) (void)lx06_dialog_light(LX06_LIGHT_THINKING);
            if (event.text[0]) *last_activity = now;
            if (event.text[0]) xz_talk_log(g_talk, "user", event.text);
            if (!speaking && !goodbye->pending && !goodbye->sent_ms &&
                (!g_listen_pause_ms || session->state == XZ_WAIT_RESPONSE)) {
                int result = xz_home_mcp_voice(home, ws, event.text, now);
                if (result) {
                    const char *event_name = result == 2 ? "HOME_MCP_VOICE_REUSED" :
                        result == 1 ? "HOME_MCP_VOICE_SUBMITTED" : "HOME_MCP_PROVIDER_UNAVAILABLE";
                    emit(timeline, event_name, "");
                }
            }
            if ((g_keyword_mode || g_human_mode) && !g_listen_pause_ms && event.text[0]) {g_waiting=1;g_reply_serial++;}
            if (!g_keyword_mode && !goodbye->pending && goodbye->phrase && xz_text_matches_list(event.text, goodbye->phrase)) {
                goodbye->pending = 1;
                goodbye->seen_ms = now;
                emit(timeline, "GOODBYE_WORD_DETECTED", "text_bytes=%zu", event.text_bytes);
                if(*env_or("XZ_BARGE_ANY","")) {
                    abort_player(player,player_ready);
                    *player_active=*tts_active=*playback_draining=0;
                    pending_playback_bytes=0;g_waiting=0;
                    return send_goodbye(ws,session,timeline,goodbye)?-1:1;
                }
            }
            if (*tts_active && *pending_barge && barge_enabled) {
                char abort_json[256];
                if (xz_protocol_build_abort(abort_json, sizeof(abort_json),
                                            session->session_id,
                                            "wake_word_detected") != 0 ||
                    xz_ws_send_text(ws, abort_json) != 0) {
                    emit(timeline, "BARGE_ABORT_SEND_ERROR", "");
                    return -1;
                }
                abort_player(player, player_ready);
                *player_active = 0;
                if (xz_session_abort(session) != 0) return -1;
                *tts_active = 0;
                *pending_barge = 0;
                xz_barge_gate_reset(barge_gate);
                emit(timeline, "BARGE_DECISION", "accepted=1 text_bytes=%zu", event.text_bytes);
                emit(timeline, "ABORT_SENT", "reason=external_speech");
                g_discard_reply_tail=1;
                (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                if (send_listen_start(ws, timeline, session->session_id) != 0) return -1;
            }
            break;
        }
        case XZ_EVENT_LLM:
            emit(timeline, "LLM_EVENT", "text_bytes=%zu emotion=%s",
                 event.text_bytes, event.emotion[0] ? event.emotion : "none");
            break;
        case XZ_EVENT_MCP:
            if (!home->provider) {
                emit(timeline, "MCP_UNSUPPORTED", "");
                break;
            }
            emit(timeline, "HOME_MCP_MESSAGE", "");
            return xz_home_mcp_request(home, ws, session->session_id, (const char *)data,
                                       length, monotonic_ms(), goodbye->pending || goodbye->sent_ms);
        case XZ_EVENT_ABORT:
            if (g_human_mode && (g_discard_reply_tail ||
                !strcmp(event.reason, "speech_detected"))) break;
            g_interrupt_consumed=1;
            g_discard_reply_tail=1;
            if (*tts_active || *player_active || *playback_draining || pending_playback_bytes) {
                abort_player(player, player_ready);
                *player_active = 0;
                (void)xz_session_abort(session);
                *tts_active = 0;
                *playback_draining = 0;
            }
            *pending_barge = 0;
            xz_barge_gate_reset(barge_gate);
            emit(timeline, "SERVER_ABORT", "reason=%s", event.reason[0] ? event.reason : "none");
            (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
            if (send_listen_start(ws, timeline, session->session_id) != 0) return -1;
            break;
        case XZ_EVENT_ERROR:
            emit(timeline, "SERVER_ERROR", "code=%s", event.reason[0] ? event.reason : "unknown");
            break;
        case XZ_EVENT_GOODBYE:
            emit(timeline, "GOODBYE", "");
            return 1;
        default:
            emit(timeline, "SERVER_EVENT_UNKNOWN", "");
            break;
    }
    return 0;
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    FILE *timeline_stream = open_timeline();
    if (!timeline_stream) {
        fputs("cannot open XZ_TIMELINE_FILE\n", stderr);
        return 2;
    }
    if (timeline_stream != stderr && timeline_stream != stdout) {
        g_xfsz_fd = fileno(timeline_stream);
        /* POSIX-only ARM builds resolve signal() to __sysv_signal, whose
         * handler resets after one delivery. Keep the fallback persistent. */
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_xfsz;
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGXFSZ, &action, NULL) != 0) return 2;
    }
    xz_timeline timeline;
    xz_timeline_init(&timeline, timeline_stream);
    char timeline_archive[1024];
    const char *timeline_path = getenv("XZ_TIMELINE_FILE");
    if (timeline_stream != stdout && timeline_stream != stderr && timeline_path &&
        snprintf(timeline_archive, sizeof(timeline_archive), "%s.previous", timeline_path) < (int)sizeof(timeline_archive))
        timeline.archive_path = timeline_archive;

    /* 对话日志（用户说了什么 / 小智答了什么）：48KB 硬上限自动转，
     * 不会写满系统空间；XZ_TALK_LOG=0 可禁用。正常退出时 libc 刷新
     * stdio（armhf 链接 shim 无 atexit 符号，故不注册 atexit）。 */
    g_talk = xz_talk_open(env_or("XZ_TALK_LOG", "/data/lx06/logs/xiaozhi.talk.log"));
    FILE *talk_stream = xz_talk_stream(g_talk);
    if (talk_stream) g_talk_fd = fileno(talk_stream);

    const char *url = env_or("XZ_WS_URL", "wss://YOUR_XIAOZHI_HOST/xiaozhi/v1/");
    const char *client_id = getenv("XZ_CLIENT_ID");
    char generated_client_id[37];
    if (!client_id || !*client_id) {
        const char *client_id_file = env_or("XZ_CLIENT_ID_FILE", "/data/lx06/xiaozhi/client_id");
        if (xz_client_id_load_or_create(client_id_file, generated_client_id,
                                        sizeof(generated_client_id)) != 0) {
            emit(&timeline, "CONFIG_ERROR", "client_id_unavailable");
            if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
            return 2;
        }
        client_id = generated_client_id;
    }
    const char *token = getenv("XZ_ACCESS_TOKEN");
    const char *wake_text = getenv("XZ_WAKE_TEXT");
    if(!wake_text)wake_text="小爱同学召唤小智";
    const char *goodbye_text = env_or("XZ_GOODBYE_TEXT", "小智再见");
    const char *interrupt_text = env_or("XZ_INTERRUPT_TEXT", "");
    g_interrupt_list = interrupt_text;
    g_open_list = env_or("XZ_OPEN_TEXT", "");
    g_phrase_uplink = interrupt_text[0] != 0;
    g_listen_mode = g_phrase_uplink ? "realtime" : "auto";
    const char *mic_path = env_or("XZ_MIC_FIFO", "/tmp/mico-voip-audio.fifo");
    const char *aplay = env_or("XZ_APLAY", "aplay");
    const char *alsa_device = env_or("XZ_ALSA_DEVICE", "default");
    int playback_enabled = env_enabled("XZ_PLAYBACK", 1);
    int local_aec_enabled = env_enabled("XZ_LOCAL_AEC", 0);
    /* Default to half-duplex: never offer the device's active TTS to the server. */
    int full_duplex = env_enabled("XZ_FULL_DUPLEX", 0);
    int barge_enabled = env_enabled("XZ_BARGE_IN", 0);
    /* New explicit dual-mode setting. Unset retains the installed legacy
     * behavior, so old scripts aren't silently changed by a client update. */
    const char *barge_mode=getenv("XZ_BARGE_ANY");
    if(barge_mode && *barge_mode) {
        if(strcmp(barge_mode,"0") && strcmp(barge_mode,"1")) {
            emit(&timeline,"CONFIG_ERROR","XZ_BARGE_ANY_must_be_0_or_1");return 2;
        }
        g_keyword_mode=!strcmp(barge_mode,"0");
        g_human_mode=!g_keyword_mode;
        g_phrase_uplink=0;
        full_duplex=0;
        barge_enabled=0;
        g_listen_mode="auto";
        emit(&timeline,"INTERRUPTION_MODE","XZ_BARGE_ANY=%s provider=speaker_xiaomi_asr",barge_mode);
    }
    g_listen_pause_ms = (unsigned)env_long("XZ_LISTEN_PAUSE_MS", 0, 0, 8000);
    if (g_listen_pause_ms >= 1000) {
        g_listen_mode = "manual";
        emit(&timeline, "MANUAL_LISTEN", "pause_ms=%u", g_listen_pause_ms);
        g_manual_end_word = env_or("XZ_LISTEN_END_WORD", "");
    } else {
        g_listen_pause_ms = 0;
    }
    int timeout_ms = (int)env_long("XZ_IDLE_TIMEOUT_MS", 120000, 0, 86400000);
    unsigned keepalive_minutes = (unsigned)env_long("XZ_KEEPALIVE_MINUTES", 0, 0, 120);
    if (keepalive_minutes) timeout_ms = 0; /* The script's maximum session duration owns expiry. */
    const char *task_query = env_or("XZ_MCP_TASK_QUERY", "");
    int task_enabled = env_long("XZ_MCP_TASK_AUTO", *task_query != 0, 0, 1) != 0;
    const char *task_root = env_or("XZ_MCP_TASK_ROOT", XZ_TASK_ROOT);
    unsigned aec_output_delay_ms = (unsigned)env_long("XZ_AEC_OUTPUT_DELAY_MS", 90, 0, 1000);
    unsigned barge_min_rms = (unsigned)env_long("XZ_BARGE_MIN_RMS", 900, 100, 12000);
    xz_input_gain input_gain;
    xz_input_gain_init_db(&input_gain,getenv("XZ_INPUT_GAIN_DB"),getenv("XZ_INPUT_GAIN"));
    emit(&timeline,"INPUT_GAIN","db=%.2f preset=%u scope=listening_uplink",input_gain.db,input_gain.preset);
    char device_id[128];

    if (!client_id || !*client_id || read_device_id(device_id, sizeof(device_id)) != 0) {
        emit(&timeline, "CONFIG_ERROR", "missing_device_or_client_id");
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 2;
    }
    if (barge_enabled && (!local_aec_enabled || !playback_enabled || !full_duplex)) {
        emit(&timeline, "CONFIG_ERROR", "barge_in_requires_aec_playback_and_full_duplex");
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 2;
    }

    emit(&timeline, "CLIENT_START", "protocol=1 mic_rate=16000 frame_ms=60 duplex=%d playback=%d local_aec=%d barge=%d phrase_uplink=%d listen=%s",
         full_duplex, playback_enabled, local_aec_enabled, barge_enabled,
         g_phrase_uplink, g_listen_mode);

    xz_ws ws;
    xz_session session;
    xz_home_mcp home = {0};
    if (env_enabled("XZ_HOME_MCP", 0))
        xz_home_mcp_configure(&home, getenv("XZ_HOME_MCP_SCRIPT"));
    emit(&timeline, "HOME_MCP_CONFIG", "provider_ready=%d", home.provider != NULL);
    unsigned char message[XZ_MESSAGE_CAP];
    size_t message_length = 0;
    int message_type = 0;
    char hello_json[512];
    int result = 5;
    int connect_timeout_ms = (int)env_long("XZ_CONNECT_TIMEOUT_MS", 10000, 1000, 60000);
    /* Local echo cancellation does not require the gateway's AEC extension.
     * Keep realtime's existing negotiation and leave keyword mode independent. */
    int server_aec_enabled = env_enabled("XZ_SERVER_AEC", g_phrase_uplink);
    if (xz_protocol_build_hello(hello_json, sizeof(hello_json), server_aec_enabled, home.provider != NULL) != 0) {
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 4;
    }

    /* Retry at most once, before opening the microphone or sending a wake
     * phrase. A slow or silent gateway cannot replay conversation audio. */
    for (unsigned attempt = 0; attempt < 2 && !g_stop_requested; ++attempt) {
        if (xz_ws_connect(&ws, url, device_id, client_id, token, connect_timeout_ms) != 0) {
            emit(&timeline, "WS_CONNECT_ERROR", "attempt=%u", attempt + 1);
            result = 3;
            break;
        }
        emit(&timeline, "WS_CONNECTED", "tls=%d attempt=%u", ws.tls, attempt + 1);
        xz_session_init(&session);
        xz_session_connected(&session);
        if (send_text(&ws, &timeline, "HELLO_SENT", hello_json) != 0) {
            xz_ws_close(&ws);
            result = 4;
            break;
        }

        uint64_t hello_deadline = monotonic_ms() + (attempt == 0 ? 3500 : 10000);
        int protocol_error = 0;
        while (!g_stop_requested && monotonic_ms() < hello_deadline) {
            int rc = xz_ws_receive(&ws, &message_type, message, sizeof(message),
                                   &message_length, 100);
            if (rc < 0 || (rc > 0 && message_type == XZ_WS_CLOSED)) break;
            if (rc == 0) continue;
            if (message_type != XZ_WS_TEXT) {
                emit(&timeline, "PRE_HELLO_BINARY_REJECTED", "bytes=%zu", message_length);
                protocol_error = 1;
                break;
            }
            xz_hello hello;
            char server_session_id[96] = "";
            if (xz_protocol_parse_server_hello((const char *)message, message_length,
                                               &hello, server_session_id,
                                               sizeof(server_session_id)) != 0 ||
                xz_session_accept_hello(&session, &hello) != 0) {
                emit(&timeline, "HELLO_REJECTED", "");
                protocol_error = 1;
                break;
            }
            result = 0;
            emit(&timeline, "HELLO_OK", "version=%d server_rate=%d session_id_bytes=%zu",
                 hello.version, hello.sample_rate, strlen(server_session_id));
            break;
        }
        if (!result) break;
        xz_ws_close(&ws);
        if (protocol_error || g_stop_requested || attempt != 0) break;
        emit(&timeline, "HELLO_RETRY", "attempt=2");
    }
    if (result) {
        if (result == 5) emit(&timeline, "HELLO_TIMEOUT_OR_INVALID", "");
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return result;
    }

    xz_opus_codec codec;
    if (xz_opus_codec_init(&codec, XZ_MIC_RATE, session.server_sample_rate) != 0) {
        emit(&timeline, "OPUS_INIT_ERROR", "server_rate=%d", session.server_sample_rate);
        xz_ws_close(&ws);
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 6;
    }

    xz_aec aec;
    memset(&aec, 0, sizeof(aec));
    if (local_aec_enabled) {
        size_t delay_samples = (size_t)env_long("XZ_AEC_DELAY_MS", 5, 0, 60) * XZ_MIC_RATE / 1000;
        if (!playback_enabled || xz_aec_init(&aec, XZ_MIC_RATE, 128,
                                             delay_samples, 0.15f) != 0) {
            emit(&timeline, "AEC_INIT_ERROR", "");
            xz_opus_codec_destroy(&codec);
            xz_ws_close(&ws);
            if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
            return 7;
        }
    }
    xz_barge_gate barge_gate;
    xz_barge_gate_init(&barge_gate, 3, barge_min_rms);

    int is_fifo = 0;
    int mic_fd = wait_for_microphone(mic_path,
        (int)env_long("XZ_MIC_WAIT_MS", 15000, 0, 120000), &is_fifo);
    if (mic_fd < 0) {
        emit(&timeline, "MIC_OPEN_ERROR", "fifo_path_configured=1");
        xz_aec_destroy(&aec);
        xz_opus_codec_destroy(&codec);
        xz_ws_close(&ws);
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 8;
    }
    emit(&timeline, "MIC_OPEN", "source=%s", is_fifo ? "fifo" : "file");

    char task_session_dir[XZ_TASK_PATH_MAX] = {0};
    if (task_enabled && xz_task_session_open(task_root, task_session_dir)) {
        emit(&timeline, "MCP_TASK_SESSION_ERROR", "");
        task_enabled = 0;
    }
    char json[512];
    if ((*wake_text&&(xz_protocol_build_detect(json,sizeof json,session.session_id,wake_text)!=0||
                      send_text(&ws,&timeline,"WAKE_DETECT",json)!=0))||
        send_listen_start(&ws,&timeline,session.session_id)!=0) {
        xz_task_session_close(task_session_dir);
        close(mic_fd);
        xz_aec_destroy(&aec);
        xz_opus_codec_destroy(&codec);
        xz_ws_close(&ws);
        if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
        return 9;
    }

    (void)lx06_dialog_light(LX06_LIGHT_LISTENING);
    xz_player player;
    memset(&player, 0, sizeof(player));
    int player_ready = 0;
    int player_active = 0;
    int tts_active = 0;
    int playback_draining = 0;
    int pending_barge = 0;
    int mic_eof = 0;
    int partial_reported = 0;
    uint64_t frames_sent = 0;
    uint64_t audio_rx_frames = 0;
    uint64_t last_activity = monotonic_ms();
    uint64_t next_ping = last_activity + XZ_KEEPALIVE_INTERVAL_MS;
    xz_task_poll task_poll = {0};
    uint64_t task_quiet_since = 0, latest_task_mic = 0;
    uint64_t echo_until_ms = 0;
    uint64_t playback_tail_until_ms = 0;
    char spoken_text[1024] = "";
    xz_goodbye_state goodbye;
    memset(&goodbye, 0, sizeof(goodbye));
    goodbye.phrase = goodbye_text;
    unsigned char mic_pending[XZ_MIC_BYTES];
    size_t mic_pending_bytes = 0;
    int16_t mic_pcm[XZ_MIC_SAMPLES];
    int16_t processed_pcm[XZ_MIC_SAMPLES];
    int16_t reference_pcm[XZ_MIC_SAMPLES];
    int16_t decoded_pcm[XZ_MAX_SERVER_SAMPLES];
    unsigned char opus_packet[XZ_OPUS_PACKET_CAP];
    xz_reference_ring reference_ring;
    memset(&reference_ring, 0, sizeof(reference_ring));
    int output_rate = session.server_sample_rate;
    uint64_t idle_timeout_ms = timeout_ms > 0 ? (uint64_t)timeout_ms : 0;
    int exit_status = 0;
    int finished = 0;
    lx06_control_asr *control_asr=NULL;
    if(g_keyword_mode || (g_human_mode && playback_enabled) || *g_manual_end_word) {
        control_asr=lx06_control_asr_open(env_or("LX06_CONTROL_ASR_SOCKET",""));
        if(!control_asr && g_keyword_mode){emit(&timeline,"CONTROL_ASR_UNAVAILABLE","protected_mode_requires_bridge");finished=1;exit_status=16;}
        if(!control_asr && *g_manual_end_word)emit(&timeline,"MANUAL_END_WORD_UNAVAILABLE","fallback=silence");
        if(g_keyword_mode)g_waiting=*wake_text!=0;
    }
    db_echo human_echo = {0};
    xz_human_barge *human_barge = NULL;
    if (g_human_mode && playback_enabled) {
        human_echo.speech_rms = (unsigned)env_long("XZ_BARGE_RMS", XZ_BARGE_DEFAULT_RMS,
                                                  DB_BARGE_MIN_RMS, DB_BARGE_MAX_RMS);
        human_echo.require_reference = 1;
        if (control_asr && (human_barge = calloc(1, sizeof(*human_barge))) &&
            pthread_mutex_init(&human_echo.lock, NULL) != 0) {
            free(human_barge);
            human_barge = NULL;
        }
        if (human_barge) emit(&timeline, "HUMAN_BARGE_READY", "rms=%u", human_echo.speech_rms);
        else emit(&timeline, "HUMAN_BARGE_UNAVAILABLE", "fallback=protected_half_duplex");
    }
    db_echo *playback_echo = human_barge ? &human_echo : NULL;
    if (control_asr && *g_manual_end_word) {
        g_manual_audio = calloc(1, sizeof(*g_manual_audio));
        if (g_manual_audio) g_manual_audio->cue_at = UINT64_MAX;
        else emit(&timeline, "MANUAL_END_WORD_UNAVAILABLE", "fallback=silence");
    }
    int auxiliary_active = 0;

    while (!g_stop_requested && !finished) {
        int home_pending = home.pid != 0;
        if (xz_home_mcp_poll(&home, &ws, session.session_id, monotonic_ms()) < 0) {
            exit_status = 11;
            break;
        }
        if (home_pending && !home.pid) {
            emit(&timeline, "HOME_MCP_ACTION_RESULT", "accepted=%d", home.accepted);
        }
        int rc = xz_ws_receive(&ws, &message_type, message, sizeof(message),
                               &message_length, 5);
        if (rc < 0) {
            emit(&timeline, "WS_RECEIVE_ERROR", "");
            exit_status = 10;
            break;
        }
        if (rc > 0) {
            if (message_type == XZ_WS_CLOSED) {
                emit(&timeline, "WS_CLOSED", "");
                break;
            }
            if (message_type == XZ_WS_TEXT) {
                int event_result = handle_server_text(&ws, &session, &timeline,
                    message, message_length, &tts_active, spoken_text,
                    sizeof(spoken_text), &echo_until_ms, &pending_barge,
                    barge_enabled, &barge_gate, &player, &player_ready, &player_active,
                    &playback_draining, &playback_tail_until_ms, g_listen_mode, &goodbye,
                    &last_activity, &home);
                if (event_result < 0) {
                    exit_status = 11;
                    break;
                }
                if (event_result > 0) finished = 1;
            } else if (message_type == XZ_WS_BINARY && !g_discard_reply_tail) {
                int samples = xz_opus_decode(&codec, message, message_length,
                                             decoded_pcm, XZ_MAX_SERVER_SAMPLES);
                if (samples <= 0) {
                    emit(&timeline, "TTS_OPUS_DECODE_ERROR", "packet_bytes=%zu", message_length);
                    continue;
                }
                audio_rx_frames++;
                last_activity = monotonic_ms();
                if (playback_enabled) {
                    if (pending_playback_bytes) {
                        if (queue_pending_playback(decoded_pcm, (size_t)samples * sizeof(int16_t))) {
                            emit(&timeline, "PLAYBACK_PENDING_OVERFLOW", ""); exit_status=15; break;
                        }
                    } else if (start_player(&player, &player_ready, aplay, alsa_device,
                                     output_rate, playback_echo) != 0) {
                        emit(&timeline, "PLAYBACK_START_ERROR", "");
                    } else {
                        player_active = 1;
                        int write_result = xz_player_write(&player, decoded_pcm,
                                            (size_t)samples * sizeof(int16_t));
                        if (write_result == -2 && queue_pending_playback(decoded_pcm,
                                             (size_t)samples * sizeof(int16_t)) == 0) {
                            emit(&timeline, "PLAYBACK_DEFERRED", "samples=%d", samples);
                        } else if (write_result != 0) {
                            emit(&timeline, "PLAYBACK_QUEUE_ERROR", "samples=%d reason=%d queued=%zu capacity=%u",
                                 samples, write_result, xz_player_queued(&player), XZ_PLAYER_QUEUE_CAPACITY);
                            exit_status=15; break;
                        } else {
                            emit(&timeline, "TTS_PCM_QUEUED", "samples=%d queue_cap=%u",
                                 samples, XZ_PLAYER_QUEUE_CAPACITY);
                        }
                    }
                }
                if (local_aec_enabled && playback_enabled) {
                    reference_append(&reference_ring, decoded_pcm, (size_t)samples, output_rate);
                }
                emit(&timeline, "TTS_OPUS_RX", "packet_bytes=%zu pcm_samples=%d frame=%llu",
                     message_length, samples, (unsigned long long)audio_rx_frames);
            }
        }

        uint64_t now = monotonic_ms();
        int audible = tts_active || player_active || playback_draining || pending_playback_bytes;
        if (task_poll.awaiting_until && (audible || !g_waiting))
            task_poll.awaiting_until = 0;
        uint64_t manual_tag = g_manual_audio && g_manual_turn && !audible && !g_waiting &&
                              !goodbye.sent_ms && now >= playback_tail_until_ms ?
                              XZ_MANUAL_TURN_TAG | g_manual_turn : 0;
        int human_observe = control_asr && human_barge && player_active &&
                            !goodbye.pending && !goodbye.sent_ms;
        if (human_barge && !human_observe) {
            human_barge->used = 0;
            human_barge->last_voice = 0;
            human_barge->partial_since = 0;
        }
        int want_auxiliary = g_keyword_mode || manual_tag != 0 || human_observe;
        if (control_asr && (want_auxiliary || auxiliary_active)) {
            char control[2048];
            lx06_control_asr_reply(control_asr, manual_tag ? manual_tag : (audible||g_waiting)?g_reply_serial:0);
            int got = lx06_control_asr_feed(control_asr,want_auxiliary,NULL,0,now);
            auxiliary_active = want_auxiliary;
            if (!got && want_auxiliary)
                got = lx06_control_asr_poll(control_asr,control,sizeof(control),now);
            if (!got && manual_tag && manual_cue_is_held() && g_manual_quiet_ms >= XZ_MANUAL_WORD_QUIET_MS) {
                uint64_t partial_tag;
                const char *partial = lx06_control_asr_partial(control_asr, &partial_tag);
                if (partial_tag == manual_tag && xz_text_is_phrase(partial, g_manual_end_word) &&
                    lx06_control_asr_end_utterance(control_asr, manual_tag, now) < 0)
                    got = -1;
            }
            if(got<0 && g_keyword_mode){emit(&timeline,"CONTROL_ASR_ERROR","closing_protected_session");exit_status=16;break;}
            if (got < 0) {
                lx06_control_asr_close(control_asr);
                control_asr = NULL;
                human_observe = 0;
                if (human_barge) emit(&timeline, "HUMAN_BARGE_UNAVAILABLE", "fallback=protected_half_duplex");
                if (*g_manual_end_word) emit(&timeline, "MANUAL_END_WORD_UNAVAILABLE", "fallback=silence");
                if (g_manual_audio && send_held_audio(&ws, g_manual_audio->next)) {exit_status=12;break;}
                free(g_manual_audio);
                g_manual_audio = NULL;
            } else if (got > 0 && manual_tag && manual_cue_is_held() &&
                       lx06_control_asr_result_reply(control_asr) == manual_tag &&
                       xz_text_is_phrase(control, g_manual_end_word)) {
                g_manual_end_confirmed = 1;
                if (g_manual_quiet_ms >= XZ_MANUAL_WORD_QUIET_MS &&
                    send_manual_stop(&ws, &timeline, session.session_id, 1)) {
                    exit_status = 12;
                    break;
                }
            } else if (human_observe && human_barge->last_voice) {
                uint64_t reply = lx06_control_asr_result_reply(control_asr);
                const char *text = control;
                if (!got) text = lx06_control_asr_partial(control_asr, &reply);
                int independent = reply == g_reply_serial && *spoken_text &&
                    now - human_barge->last_voice <= 700 && xz_text_has_speech(text, got ? 2 : 4) &&
                    !xz_text_is_echo(text, spoken_text) && !xz_text_matches_phrase(spoken_text, text);
                if (!independent) human_barge->partial_since = 0;
                else if (!got && (!human_barge->partial_since || strcmp(human_barge->partial, text))) {
                    snprintf(human_barge->partial, sizeof(human_barge->partial), "%s", text);
                    human_barge->partial_since = now;
                } else if (got || now - human_barge->partial_since >= 75) {
                    int cancel_remote = tts_active;
                    abort_player(&player, &player_ready);
                    player_active = tts_active = playback_draining = g_waiting = 0;
                    playback_tail_until_ms = 0;
                    human_barge->guard_tail_until = monotonic_ms() + 460;
                    g_discard_reply_tail = 1;
                    if (session.state == XZ_SPEAKING) (void)xz_session_abort(&session);
                    else if (session.state == XZ_WAIT_RESPONSE) session.state = XZ_LISTENING;
                    pending_barge = 0;
                    xz_barge_gate_reset(&barge_gate);
                    last_activity = now;
                    (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                    emit(&timeline, "HUMAN_BARGE", "source=%s buffered_ms=%u",
                         got ? "asr_final" : "asr_partial", human_barge->used * XZ_FRAME_MS);
                    if (xz_text_matches_list(text, goodbye_text)) {
                        finished = 1;
                        if (send_goodbye(&ws, &session, &timeline, &goodbye)) exit_status = 16;
                        break;
                    }
                    char abort[256];
                    if ((cancel_remote && (xz_protocol_build_abort(abort, sizeof(abort), session.session_id,
                                                                 "wake_word_detected") ||
                                           send_text(&ws, &timeline, "ABORT_SENT", abort))) ||
                        send_listen_start(&ws, &timeline, session.session_id) ||
                        replay_barge_audio(human_barge, &ws, &codec)) {
                        exit_status = 16;
                        break;
                    }
                    if (lx06_control_asr_feed(control_asr, 0, NULL, 0, now) < 0) {
                        lx06_control_asr_close(control_asr);
                        control_asr = NULL;
                    }
                    auxiliary_active = 0;
                    human_observe = audible = 0;
                }
            } else if (got > 0 && g_keyword_mode) {
                emit(&timeline,"CONTROL_ASR_FINAL","text=%s",control);
                int echo=audible && spoken_text[0] && xz_self_echo(control,spoken_text,g_open_list,goodbye_text,g_interrupt_list);
                if(echo)emit(&timeline,"CONTROL_SELF_ECHO_SUPPRESSED","");
                else if(xz_text_matches_list(control,goodbye_text)) {
                    abort_player(&player,&player_ready);player_active=tts_active=playback_draining=0;pending_playback_bytes=0;g_waiting=0;
                    goodbye.pending=1;goodbye.seen_ms=now;
                    emit(&timeline,"GOODBYE_WORD_DETECTED","source=speaker_asr");
                    (void)send_goodbye(&ws,&session,&timeline,&goodbye);
                    finished=1;break;
                } else if(xz_text_matches_list(control,g_interrupt_list)) {
                    if(g_interrupt_consumed || (!audible && !g_waiting) ||
                       lx06_control_asr_result_reply(control_asr)!=g_reply_serial) {
                        emit(&timeline,"REPEATED_INTERRUPT_IGNORED","listening_unchanged");continue;
                    }
                    g_interrupt_consumed=1;
                    g_discard_reply_tail=1;
                    char abort_json[256];
                    if(xz_protocol_build_abort(abort_json,sizeof(abort_json),session.session_id,"wake_word_detected") || xz_ws_send_text(&ws,abort_json)) {exit_status=16;break;}
                    abort_player(&player,&player_ready);player_active=tts_active=playback_draining=0;pending_playback_bytes=0;g_waiting=0;
                    if(session.state==XZ_SPEAKING)(void)xz_session_abort(&session);
                    pending_barge=0;xz_barge_gate_reset(&barge_gate);last_activity=now;
                    emit(&timeline,"INTERRUPT_WORD","source=speaker_asr");
                    emit(&timeline,"ABORT_SENT","reason=interrupt_word");
                    (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                    if(send_listen_start(&ws,&timeline,session.session_id)){exit_status=16;break;}
                } else emit(&timeline,"CONTROL_NON_WORD_IGNORED","text_bytes=%zu",strlen(control));
            }
        }
        if (!mic_eof && !g_stop_requested) {
            ssize_t got = read(mic_fd, mic_pending + mic_pending_bytes,
                               sizeof(mic_pending) - mic_pending_bytes);
            if (got > 0) {
                mic_pending_bytes += (size_t)got;
                if (mic_pending_bytes == sizeof(mic_pending)) {
                    uint64_t capture_end_ms = monotonic_ms();
                    uint64_t capture_start_ms = capture_end_ms >= XZ_FRAME_MS ?
                                                capture_end_ms - XZ_FRAME_MS : 0;
                    memcpy(mic_pcm, mic_pending, sizeof(mic_pcm));
                    memcpy(processed_pcm, mic_pcm, sizeof(processed_pcm));
                    double echo_correlation = 1.0;
                    unsigned rms = 0;
                    if (local_aec_enabled && !g_human_mode &&
                        (tts_active || player_active || playback_draining)) {
                        reference_frame(&reference_ring, capture_start_ms,
                                        aec_output_delay_ms, reference_pcm);
                        if (xz_aec_process(&aec, reference_pcm, mic_pcm,
                                           processed_pcm, XZ_MIC_SAMPLES) != 0) {
                            emit(&timeline, "AEC_PROCESS_ERROR", "");
                        }
                        echo_correlation = aec.last_correlation;
                    }
                    if (human_barge) {
                        /* Never feed speaker-dominated frames to either recognizer.
                         * The short lead buffer preserves the beginning of speech. */
                        int protect = human_observe || capture_end_ms < human_barge->guard_tail_until;
                        for (size_t offset = 0; offset < sizeof(processed_pcm); offset += DB_ECHO_FRAME_BYTES)
                            db_echo_filter(&human_echo, mic_pending + offset, protect, capture_end_ms,
                                           (unsigned char *)processed_pcm + offset);
                        if (human_observe) {
                            if (frame_speech_like(processed_pcm, frame_rms(processed_pcm)))
                                human_barge->last_voice = capture_end_ms;
                            memcpy(human_barge->pcm[human_barge->next], processed_pcm, sizeof(processed_pcm));
                            human_barge->next = (human_barge->next + 1) % XZ_BARGE_HISTORY;
                            unsigned capacity = human_barge->last_voice ? XZ_BARGE_HISTORY : 2;
                            if (human_barge->used < capacity) ++human_barge->used;
                        }
                    }
                    rms = frame_rms(processed_pcm);
                    int speech_like = frame_speech_like(processed_pcm, rms);
                    if (tts_active && local_aec_enabled &&
                        xz_barge_gate_observe(&barge_gate, 1, rms,
                                              (float)echo_correlation, speech_like)) {
                        pending_barge = 1;
                        emit(&timeline, "BARGE_CANDIDATE", "rms=%u corr=%.3f speech_like=%d",
                             rms, echo_correlation, speech_like);
                    }
                    const int16_t *uplink_pcm = processed_pcm;
                    int protected_audio=(g_keyword_mode || g_human_mode) && (audible || g_waiting);
                    /* Native realtime also uploads microphone frames while
                     * listening. After the actual player exits, replace tail
                     * frames with silence for BOTH recognizers. Use the frame
                     * start so a frame straddling the deadline cannot leak
                     * the room's last playback samples. Keep the cadence and
                     * connection alive; never delay or replay these frames. */
                    int tail_protected = !tts_active && !player_active &&
                        !pending_playback_bytes && playback_tail_until_ms &&
                        capture_start_ms < playback_tail_until_ms;
                    if (task_enabled) {
                        latest_task_mic = capture_end_ms;
                        if (audible || tail_protected || speech_like || g_waiting) {
                            task_quiet_since = 0;
                        } else if (!task_quiet_since) {
                            task_quiet_since = capture_start_ms;
                        }
                        /* A status query may be dropped to accept the caller's
                           first voiced frame while the remote reply is pending. */
                        if (task_poll.awaiting_until && !audible && !tail_protected && speech_like) {
                            if (cancel_task_query(&ws, &timeline, session.session_id, &task_poll,
                                                  "MCP_TASK_YIELD_TO_SPEECH") != 0) {
                                exit_status = 12;
                                break;
                            }
                            protected_audio = 0;
                        }
                    }
                    if (tail_protected) memset(processed_pcm, 0, sizeof(processed_pcm));
                    int manual_listening = g_listen_pause_ms && !audible &&
                        !g_waiting && !tail_protected && !goodbye.sent_ms;
                    int auxiliary_error = 0;
                    if (control_asr && manual_listening && g_manual_audio && speech_like &&
                        g_manual_quiet_ms >= XZ_MANUAL_WORD_GAP_MS) {
                        /* Recognize the new segment independently, with two
                           pre-roll frames held before its first voiced frame. */
                        g_manual_audio->cue_at = g_manual_audio->next - 2;
                        g_manual_end_confirmed = 0;
                        ++g_manual_turn;
                        lx06_control_asr_reply(control_asr, XZ_MANUAL_TURN_TAG | g_manual_turn);
                        auxiliary_error = lx06_control_asr_feed(control_asr,0,NULL,0,capture_end_ms);
                    }
                    if (control_asr && auxiliary_active &&
                        (auxiliary_error || lx06_control_asr_feed(control_asr,1,processed_pcm,sizeof(processed_pcm),capture_end_ms))) {
                        if (g_keyword_mode) {exit_status=16;break;}
                        lx06_control_asr_close(control_asr);
                        control_asr = NULL;
                        if (human_barge) emit(&timeline, "HUMAN_BARGE_UNAVAILABLE", "fallback=protected_half_duplex");
                        if (*g_manual_end_word) emit(&timeline, "MANUAL_END_WORD_UNAVAILABLE", "fallback=silence");
                        if (g_manual_audio && send_held_audio(&ws, g_manual_audio->next)) {exit_status=12;break;}
                        free(g_manual_audio);
                        g_manual_audio = NULL;
                    }
                    if (protected_audio || (!full_duplex && !g_phrase_uplink && (tts_active || playback_draining))) {
                        memset(processed_pcm, 0, sizeof(processed_pcm));
                        uplink_pcm = processed_pcm;
                    }
                    /* Boost only the cloud's normal listening uplink AFTER
                     * original AEC, VAD/control-word decisions and muting.
                     * Speech during playback keeps its original level, so
                     * this feature does not increase barge-in sensitivity. */
                    xz_input_gain_apply(&input_gain,processed_pcm,XZ_MIC_SAMPLES,
                        !(tts_active||player_active||playback_draining||
                          pending_playback_bytes||g_waiting||tail_protected));
                    /* The text query owns the cloud turn while the local microphone
                     * still listens for speech that can give it back to the user. */
                    if (task_poll.awaiting_until || (g_listen_pause_ms && !manual_listening)) {
                        mic_pending_bytes = 0;
                        continue;
                    }
                    int packet_length = xz_opus_encode(&codec, uplink_pcm,
                        XZ_MIC_SAMPLES, opus_packet, sizeof(opus_packet));
                    if (packet_length <= 0 ||
                        stage_manual_audio(&ws, opus_packet, (size_t)packet_length) != 0) {
                        emit(&timeline, "MIC_OPUS_SEND_ERROR", "frame=%llu",
                             (unsigned long long)(frames_sent + 1));
                        exit_status = 12;
                        break;
                    }
                    frames_sent++;
                    if (manual_listening) {
                        if (speech_like && g_manual_speech_frames)
                            last_activity = capture_end_ms;
                        if (finish_manual_turn(&ws, &timeline, session.session_id, speech_like) != 0) {
                            exit_status = 12;
                            break;
                        }
                    }
                    emit(&timeline, "MIC_FRAME", "index=%llu bytes=%u opus_bytes=%d rms=%u tts=%d aec=%d muted=%d",
                         (unsigned long long)frames_sent, (unsigned)sizeof(mic_pending),
                         packet_length, rms, tts_active, local_aec_enabled,
                         tail_protected || protected_audio ||
                         (!full_duplex && !g_phrase_uplink && (tts_active || playback_draining)));
                    mic_pending_bytes = 0;
                }
            } else if (got == 0 && !is_fifo) {
                mic_eof = 1;
                if (mic_pending_bytes && !partial_reported) {
                    emit(&timeline, "MIC_PARTIAL_FRAME_DROPPED", "bytes=%zu", mic_pending_bytes);
                    partial_reported = 1;
                }
                emit(&timeline, "MIC_EOF", "frames=%llu", (unsigned long long)frames_sent);
            } else if (got == 0 && is_fifo) {
                sleep_ms(20);
            } else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                emit(&timeline, "MIC_READ_ERROR", "errno=%d", errno);
                exit_status = 13;
                break;
            }
        }

        /* The continuous microphone and network keepalives are not user
         * activity. A long queued reply keeps its listening timeout paused
         * until the actual player has drained. A stalled TTS stream still
         * expires because only decoded audio refreshes the timestamp. */
        if (player_active || playback_draining) last_activity = monotonic_ms();
        now = monotonic_ms();
        int task_active = 0;
        if (task_enabled) {
            xz_task_poll_refresh(&task_poll, task_session_dir, now);
            if (task_poll.awaiting_until && now >= task_poll.awaiting_until &&
                cancel_task_query(&ws, &timeline, session.session_id, &task_poll,
                                  "MCP_TASK_QUERY_TIMEOUT") != 0) {
                exit_status = 12;
                break;
            }
            xz_task *next_task = xz_task_poll_next(&task_poll, task_query, now, &task_active);
            if (next_task && !g_waiting && !tts_active && !player_active &&
                !playback_draining && !pending_playback_bytes && now >= playback_tail_until_ms &&
                !goodbye.pending && !goodbye.sent_ms && !g_manual_speech_frames &&
                task_quiet_since && now - task_quiet_since >= XZ_TASK_QUIET_MS &&
                latest_task_mic && now - latest_task_mic <= 240 &&
                now - last_activity >= XZ_TASK_QUIET_MS) {
                if (send_task_query(&ws, &timeline, session.session_id, &task_poll, next_task, task_query, now) != 0) {
                    exit_status = 12;
                    break;
                }
                task_quiet_since = 0;
            }
        }
        if (idle_timeout_ms && !task_active && now - last_activity > idle_timeout_ms) {
            emit(&timeline, "IDLE_TIMEOUT", "timeout_ms=%llu",
                 (unsigned long long)idle_timeout_ms);
            break;
        }
        if ((keepalive_minutes || task_active) && now >= next_ping) {
            if (xz_ws_send_control(&ws, 9, NULL, 0) != 0) {
                emit(&timeline, "WS_KEEPALIVE_ERROR", "");
                exit_status = 10;
                break;
            }
            next_ping = now + XZ_KEEPALIVE_INTERVAL_MS;
        }
        if (player_active) {
            int exit_code = -1;
            int player_done = xz_player_poll(&player, &exit_code);
            if (player_done == 1) {
                emit(&timeline, "PLAYBACK_EXIT", "exit=%d", exit_code);
                if (exit_code != 0) emit(&timeline, "PLAYBACK_ERROR", "exit=%d", exit_code);
                xz_player_destroy(&player);
                player_ready = 0;
                player_active = 0;
                if (exit_code != 0) {
                    /* A failed ALSA child cannot consume later packets. End
                     * this session instead of spawning another failing player
                     * for every packet in the same cloud reply. */
                    pending_playback_bytes = 0;
                    exit_status = 15;
                    break;
                }
                if (playback_draining) {
                    unsigned tail_ms = (unsigned)env_long("XZ_PLAYBACK_TAIL_MUTE_MS", 500, 0, 5000);
                    playback_tail_until_ms = monotonic_ms() + tail_ms;
                    emit(&timeline,"PLAYBACK_TAIL_START","duration_ms=%u",tail_ms);
                }
            }
        }
        if (pending_playback_bytes && !player_active) {
            if (start_player(&player, &player_ready, aplay, alsa_device, output_rate, playback_echo) != 0 ||
                xz_player_write(&player, pending_playback, pending_playback_bytes) != 0) {
                emit(&timeline, "PLAYBACK_PENDING_ERROR", ""); exit_status=15; break;
            }
            emit(&timeline, "PLAYBACK_PENDING_FLUSH", "bytes=%zu", pending_playback_bytes);
            pending_playback_bytes=0;
            player_active=1;
            playback_draining=!tts_active;
            if (playback_draining) (void)xz_player_finish(&player);
        }
        if (playback_draining && !player_active &&
            monotonic_ms() >= playback_tail_until_ms) {
            playback_draining = 0;
            emit(&timeline,"PLAYBACK_TAIL_END","");
            if (resume_after_reply(&ws, &session, &timeline, &tts_active,
                                   &echo_until_ms, g_listen_mode, &goodbye) != 0) {
                exit_status = 14;
                break;
            }
        }
        /* If the goodbye phrase was heard but the server sent no reply at all,
         * end the session with the protocol goodbye ourselves. */
        if (goodbye.pending && !tts_active && !player_active && !playback_draining &&
            monotonic_ms() - goodbye.seen_ms >=
                (uint64_t)env_long("XZ_GOODBYE_WAIT_MS", 5000, 0, 30000)) {
            if (send_goodbye(&ws, &session, &timeline, &goodbye) != 0) {
                exit_status = 14;
                break;
            }
        }
        /* If the server neither answered the goodbye nor closed the socket,
         * close locally after a bounded wait: the phrase always ends the
         * session even against an unresponsive peer. */
        if (goodbye.sent_ms &&
            monotonic_ms() - goodbye.sent_ms >=
                (uint64_t)env_long("XZ_GOODBYE_CLOSE_MS", 5000, 500, 30000)) {
            emit(&timeline, "GOODBYE_LOCAL_CLOSE", "wait_ms=%llu",
                 (unsigned long long)((uint64_t)env_long("XZ_GOODBYE_CLOSE_MS", 5000, 500, 30000)));
            break;
        }
    }

    xz_home_mcp_close(&home);
    if (!g_stop_requested && pending_playback_bytes && !tts_active && exit_status == 0) {
        finish_player(&player, &player_ready, &timeline,
                      (unsigned)env_long("XZ_PLAYBACK_DRAIN_MS", 5000, 100, 30000));
        if (pending_playback_bytes &&
            start_player(&player, &player_ready, aplay, alsa_device, output_rate, playback_echo)==0 &&
            xz_player_write(&player, pending_playback, pending_playback_bytes)==0) {
            pending_playback_bytes=0;
            player_active=1;
        }
    }
    if (!g_stop_requested && player_active && !tts_active && exit_status == 0)
        finish_player(&player, &player_ready, &timeline,
                      (unsigned)env_long("XZ_PLAYBACK_DRAIN_MS", 5000, 100, 30000));
    else if (player_ready) abort_player(&player, &player_ready);
    if (human_barge) {
        pthread_mutex_destroy(&human_echo.lock);
        free(human_barge);
    }
    xz_task_session_close(task_session_dir);
    (void)lx06_dialog_light(LX06_LIGHT_END);
    emit(&timeline, "SESSION_SUMMARY", "mic_frames=%llu tts_frames=%llu status=%d",
         (unsigned long long)frames_sent, (unsigned long long)audio_rx_frames, exit_status);

    close(mic_fd);
    lx06_control_asr_close(control_asr);
    free(g_manual_audio);
    xz_aec_destroy(&aec);
    xz_opus_codec_destroy(&codec);
    xz_ws_close(&ws);
    if (timeline_stream != stdout && timeline_stream != stderr) fclose(timeline_stream);
    return exit_status;
}
