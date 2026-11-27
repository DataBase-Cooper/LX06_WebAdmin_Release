#include "listen-window.h"
#include "channel-route.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lx06_listen_window {
    struct lx06_listen_config cfg;
    unsigned start_ms, max_ms;
    uint64_t started, speech_started;
    int speech_seen;
    int reasks;
    int asr_open;
    int finished;
    struct lx06_listen_outcome outcome;
};

static void finish(struct lx06_listen_window *w, int status, const char *prompt) {
    if (w->finished) return;
    if (w->asr_open && w->cfg.asr.close) {
        w->cfg.asr.close(w->cfg.asr.ctx);
        w->asr_open = 0;
    }
    w->finished = 1;
    w->outcome.status = status;
    if (prompt) snprintf(w->outcome.prompt, sizeof(w->outcome.prompt), "%s", prompt);
    else w->outcome.prompt[0] = 0;
    w->outcome.readers_after_close = w->cfg.asr.readers ? w->cfg.asr.readers(w->cfg.asr.ctx) : 0;
}

struct lx06_listen_window *lx06_listen_open(const struct lx06_listen_config *cfg, uint64_t now) {
    struct lx06_listen_window *w;
    if (!cfg || !cfg->asr.open || !cfg->asr.close || !cfg->asr.poll) return NULL;
    w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->cfg = *cfg;
    w->start_ms = (unsigned)lx06_window_start_ms_clamp(cfg->start_ms);
    w->max_ms = (unsigned)lx06_window_ms_clamp(cfg->max_ms ? cfg->max_ms : cfg->window_ms);
    w->started = now;
    w->outcome.match.index = -1;
    if (cfg->asr.open(cfg->asr.ctx) != 0) {
        finish(w, LX06_LISTEN_ASR_FAIL, LX06_PROMPT_ASR);
        return w;
    }
    w->asr_open = 1;
    w->outcome.status = LX06_LISTEN_OPEN;
    return w;
}

static void consider(struct lx06_listen_window *w, const char *text) {
    struct lx06_match_result mr;
    if (lx06_template_match(text, w->cfg.cands, w->cfg.cand_count, &mr) != 0) {
        finish(w, LX06_LISTEN_ASR_FAIL, LX06_PROMPT_ASR);
        return;
    }
    w->outcome.match = mr;
    if (mr.kind == LX06_MATCH_EXACT || mr.kind == LX06_MATCH_TEMPLATE) {
        lx06_phrase_normalize(text, w->outcome.text, sizeof(w->outcome.text));
        finish(w, LX06_LISTEN_HIT, "");
        return;
    }
    if (mr.kind == LX06_MATCH_REJECT || mr.kind == LX06_MATCH_AMBIGUOUS) {
        finish(w, LX06_LISTEN_REJECT, LX06_PROMPT_REJECT);
        return;
    }
    if (mr.kind == LX06_MATCH_INCOMPLETE && w->reasks == 0) {
        w->reasks = 1;
        snprintf(w->outcome.prompt, sizeof(w->outcome.prompt), "%s", LX06_PROMPT_REASK);
        /* Stay attached. The ASR contract returns the next utterance once. */
        return;
    }
    if (mr.kind == LX06_MATCH_INCOMPLETE)
        finish(w, LX06_LISTEN_MISS, LX06_PROMPT_REASK_END);
    else
        finish(w, LX06_LISTEN_MISS, LX06_PROMPT_MISS);
}

static void observe_speech(struct lx06_listen_window *w) {
    uint64_t at;
    if (!w->speech_seen && w->cfg.asr.speech_seen &&
        w->cfg.asr.speech_seen(w->cfg.asr.ctx, &at) &&
        at >= w->started && at - w->started < w->start_ms) {
        w->speech_seen = 1;
        w->speech_started = at;
    }
}

int lx06_listen_feed(struct lx06_listen_window *w, const void *pcm, size_t bytes, uint64_t now) {
    int rc;
    if (!w || w->finished || !w->asr_open) return 0;
    if (!w->cfg.asr.feed) return 0;
    rc = w->cfg.asr.feed(w->cfg.asr.ctx, pcm, bytes, now);
    observe_speech(w);
    if (rc != 0) finish(w, LX06_LISTEN_ASR_FAIL, LX06_PROMPT_ASR);
    return rc;
}

int lx06_listen_poll(struct lx06_listen_window *w, uint64_t now, struct lx06_listen_outcome *out) {
    char text[128];
    int rc;
    if (!w) return -1;
    if (!w->finished && w->asr_open) {
        observe_speech(w);
        if ((!w->speech_seen && now >= w->started + w->start_ms) ||
            (w->speech_seen && now >= w->speech_started + w->max_ms)) {
            finish(w, LX06_LISTEN_TIMEOUT, LX06_PROMPT_TIMEOUT);
        } else {
            text[0] = 0;
            rc = w->cfg.asr.poll(w->cfg.asr.ctx, text, sizeof(text), now);
            if (rc < 0) finish(w, LX06_LISTEN_ASR_FAIL, LX06_PROMPT_ASR);
            else if (rc == 1 && text[0]) {
                /* Legacy ASR adapters without PCM observation can still final
                 * and reask; production uses the actual PCM start timestamp. */
                if (!w->speech_seen) { w->speech_seen = 1; w->speech_started = now; }
                consider(w, text);
            }
            /* Empty finals never succeed or extend either deadline. */
        }
    }
    if (out) *out = w->outcome;
    return w->finished ? 1 : 0;
}

void lx06_listen_cancel(struct lx06_listen_window *w, uint64_t now) {
    (void)now;
    if (!w) return;
    finish(w, LX06_LISTEN_CANCELLED, "");
}

void lx06_listen_close(struct lx06_listen_window *w) {
    if (!w) return;
    if (!w->finished) finish(w, LX06_LISTEN_CANCELLED, "");
    free(w);
}

int lx06_listen_busy(const struct lx06_listen_window *w) {
    return w && !w->finished;
}
