#ifndef WAKE_CLOUD_VERIFY_H
#define WAKE_CLOUD_VERIFY_H
#include <stdint.h>
/* Official-session view. session_busy is an extra reason to skip, never the
 * only one: a recent native wake or a router/recognition mark skips even when
 * session-run is idle. */
struct wake_official_view {
    int native_recent;
    int native_changed;
    int router_busy;
    int session_busy;
};
enum wake_cloud_outcome {
    WAKE_CLOUD_LOCAL = 0,     /* switch off, not gray, or fallback to the local verdict */
    WAKE_CLOUD_DISPATCH,      /* ASR agrees with the local word */
    WAKE_CLOUD_REASSIGN,      /* ASR matches another near word */
    WAKE_CLOUD_DISCARD,       /* ASR text is real speech and matches no word */
    WAKE_CLOUD_SKIPPED        /* not sent: off, not gray, too long, or official busy */
};
struct wake_cloud_request {
    int enabled;
    int gray;
    int clip_samples;
    float winner_ratio;
    struct wake_official_view official;
};
/* Enrollment references. label is the display name and is never compared.
 * sample_pinyin holds canonical keys from each usable recording.
 * missing counts recordings that exist but cannot be compared. */
#define WAKE_SAMPLE_MAX 10
#define WAKE_PINYIN_CAP 96
struct wake_cloud_word {
    char label[48];
    float ratio;
    int nsamples;
    int missing;
    char sample_pinyin[WAKE_SAMPLE_MAX][WAKE_PINYIN_CAP];
};
/* 1 = send the winner window as one burst. 0 = do not call ASR. */
int wake_cloud_should_send(const struct wake_cloud_request *request);
/* Samples that may be sent. Returns -1 when the clip is longer than 1.8s or
 * the caller passed the forensics buffer instead of the winner window. */
int wake_cloud_clip_samples(int winner_samples, int forensics_samples);
/* Contiguous 16 kHz S16 scoring window only. Returns a safe send length
 * and sample offset, or -1 when quiet edges cannot be removed safely. */
int wake_cloud_scored_clip(const int16_t *pcm, int samples, int *offset);
const char *wake_cloud_send_mode(void); /* "burst"; realtime pacing is forbidden */
/* Heard text is matched against stored sample pinyin, never against label.
 * Any one usable sample hits that word. Two words hitting is a discard.
 * asr_rc: 1 text, 0 empty, -1 unavailable, -2 timeout, -3 cancelled. */
enum wake_cloud_outcome wake_cloud_classify(int asr_rc, const char *text,
    const char *winner, const struct wake_cloud_word *words, int nwords,
    char *out_label, int out_cap);
int wake_official_busy(const struct wake_official_view *view);
/* Snapshot native-wake "kind serial event" plus mtime. */
int wake_native_snapshot(const char *path, unsigned *serial, long long *mtime_sec, char *kind, int kind_cap);
int wake_native_changed(unsigned before_serial, long long before_mtime,
                        unsigned now_serial, long long now_mtime);
int wake_path_mtime(const char *path, long long *mtime_sec);
/* Pump: poll returns 1 and fills text, 0 pending, -1 error.
 * official_edge returns 1 when mipns/native-wake moved. cancel must send C. */
struct wake_cloud_pump {
    int (*poll_text)(void *ctx, char *text, int cap);
    int (*official_edge)(void *ctx);
    void (*cancel)(void *ctx);
    void *ctx;
    int elapsed_ms;
    int step_ms;
};
int wake_cloud_pump(struct wake_cloud_pump *pump, int budget_ms, char *text, int cap);
#endif
