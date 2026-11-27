/* Listen window (§2.3). control_asr is attached only between open and the
 * matching close. WT2's control window uses this same object with its own
 * candidate list and its own timeout; this module does not know any app. */
#ifndef LX06_LISTEN_WINDOW_H
#define LX06_LISTEN_WINDOW_H
#include "template-matcher.h"
#include <stddef.h>
#include <stdint.h>

enum lx06_listen_status {
    LX06_LISTEN_OPEN = 0,
    LX06_LISTEN_HIT = 1,
    LX06_LISTEN_TIMEOUT = 2,
    LX06_LISTEN_ASR_FAIL = 3,
    LX06_LISTEN_MISS = 4,
    LX06_LISTEN_REJECT = 5,
    LX06_LISTEN_CANCELLED = 6
};

struct lx06_listen_asr {
    void *ctx;
    int (*open)(void *ctx); /* 0 attached, -1 unavailable. Bumps the reader count. */
    void (*close)(void *ctx);
    int (*feed)(void *ctx, const void *pcm, size_t bytes, uint64_t now);
    /* 1 = one final utterance in buf, 0 = still listening, -1 = failed.
     * A final is consumed: the next poll returns 0 until a new utterance. */
    int (*poll)(void *ctx, char *buf, size_t cap, uint64_t now);
    int (*readers)(void *ctx);
    /* Optional: 1 once PCM speech starts; writes its monotonic timestamp.
     * Keeps the actual start time even if the next poll is delayed. */
    int (*speech_seen)(void *ctx, uint64_t *started);
};

struct lx06_listen_config {
    const struct lx06_match_candidate *cands; /* caller keeps them alive */
    int cand_count;
    unsigned window_ms; /* compatibility post-speech max, used if max_ms is 0 */
    struct lx06_listen_asr asr;
    unsigned start_ms; /* 0 = 5000, then clamped to 1000..15000 */
    unsigned max_ms; /* 0 = window_ms or 10000, clamped to 3000..15000 */
};

struct lx06_listen_outcome {
    int status;
    struct lx06_match_result match;
    char prompt[96];
    char text[128]; /* normalized heard sentence; only set on HIT; never logged here */
    int readers_after_close;
};

struct lx06_listen_window;

struct lx06_listen_window *lx06_listen_open(const struct lx06_listen_config *cfg, uint64_t now);
int lx06_listen_feed(struct lx06_listen_window *w, const void *pcm, size_t bytes, uint64_t now);
/* 1 = outcome is final. 0 = still inside the window. */
int lx06_listen_poll(struct lx06_listen_window *w, uint64_t now, struct lx06_listen_outcome *out);
void lx06_listen_cancel(struct lx06_listen_window *w, uint64_t now);
void lx06_listen_close(struct lx06_listen_window *w);
int lx06_listen_busy(const struct lx06_listen_window *w);
#endif
