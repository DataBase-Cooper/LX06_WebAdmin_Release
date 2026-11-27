#include "wake_cloud_verify.h"
#include "wake_pinyin.h"
#include "wake_policy.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int wake_official_busy(const struct wake_official_view *view) {
    if (!view) return 1;
    return view->native_recent || view->native_changed || view->router_busy || view->session_busy;
}

int wake_cloud_clip_samples(int winner_samples, int forensics_samples) {
    if (winner_samples < 8000 || winner_samples > WAKE_CLOUD_MAX_SAMPLES) return -1;
    /* The ±2s evidence buffer is never the verify payload. */
    if (forensics_samples > 0 && winner_samples == forensics_samples &&
        winner_samples >= 32000) return -1;
    return winner_samples;
}

const char *wake_cloud_send_mode(void) { return "burst"; }

int wake_cloud_scored_clip(const int16_t *pcm, int samples, int *offset) {
    enum { frame = 320, pad = 3 * frame, longest_window = 40000 };
    int first = -1, end = 0, voiced = 0;
    if (!offset) return -1;
    *offset = 0;
    if (!pcm || samples > longest_window) return -1;
    if (samples <= WAKE_CLOUD_MAX_SAMPLES) return wake_cloud_clip_samples(samples, 0);
    if (samples % frame) return -1;

    /* Keep every nonquiet block, even isolated peaks and the gaps between
     * syllables. The 60 RMS / 180 peak quiet limits and 60 ms margins match
     * enrollment's conservative edge protection. Never choose a loudest
     * subsection of an utterance that cannot fit as a whole. */
    for (int i = 0; i < samples; i += frame) {
        int peak = 0;
        int64_t energy = 0;
        for (int j = 0; j < frame; ++j) {
            int value = pcm[i + j];
            int magnitude = value < 0 ? -value : value;
            if (magnitude > peak) peak = magnitude;
            energy += (int64_t)value * value;
        }
        if (energy <= 60 * 60 * frame && peak <= 180) continue;
        if (first < 0) first = i;
        end = i + frame;
        if (energy > 150 * 150 * frame) ++voiced;
    }
    if (voiced < 3) return -1;
    first = first > pad ? first - pad : 0;
    end = end + pad < samples ? end + pad : samples;
    if (wake_cloud_clip_samples(end - first, 0) < 0) {
        fprintf(stderr,"CLOUD_CROP rejected window=%d first=%d end=%d voiced_blocks=%d\n",
                samples,first,end,voiced);
        return -1;
    }
    *offset = first;
    return end - first;
}

int wake_cloud_should_send(const struct wake_cloud_request *request) {
    if (!request || !request->enabled || !request->gray) return 0;
    if (wake_cloud_clip_samples(request->clip_samples, 0) < 0) return 0;
    if (wake_official_busy(&request->official)) return 0;
    return 1;
}

static int word_sample_hit(const char *key, const struct wake_cloud_word *word) {
    int s, n;
    if (!word) return 0;
    n = word->nsamples;
    if (n > WAKE_SAMPLE_MAX) n = WAKE_SAMPLE_MAX;
    for (s = 0; s < n; s++) {
        if (wake_pinyin_key_equal(key, word->sample_pinyin[s])) return 1;
    }
    return 0;
}

enum wake_cloud_outcome wake_cloud_classify(int asr_rc, const char *text,
    const char *winner, const struct wake_cloud_word *words, int nwords,
    char *out_label, int out_cap) {
    char spelled[WAKE_PINYIN_CAP];
    int i, hits = 0, hit = -1, heard_n, winner_i = -1;
    if (out_label && out_cap) out_label[0] = 0;
    if (asr_rc != 1 || !text || !text[0] || !words || nwords < 1) return WAKE_CLOUD_LOCAL;
    if (nwords > 16) nwords = 16;
    /* Unknown ideograph, punctuation-only, or a one-syllable fragment cannot
     * confirm or reject a word. The typed label is not consulted. */
    heard_n = wake_pinyin_normalize(text, spelled, sizeof(spelled));
    if (heard_n < 2) return WAKE_CLOUD_LOCAL;
    for (i = 0; i < nwords; i++) {
        if (winner && !strcmp(words[i].label, winner)) winner_i = i;
        if (!word_sample_hit(spelled, &words[i])) continue;
        hits++;
        hit = i;
    }
    if (hits > 1) return WAKE_CLOUD_DISCARD;
    if (hits == 1 && hit >= 0) {
        if (winner && !strcmp(words[hit].label, winner)) {
            if (out_label && out_cap) snprintf(out_label, (size_t)out_cap, "%s", winner);
            return WAKE_CLOUD_DISPATCH;
        }
        /* Near-band only. A far acoustic word must not steal the wake, and
         * the acoustic winner must not pass on another word's pronunciation. */
        if (words[hit].ratio <= 1.0f + WAKE_NORM_BAND) {
            if (out_label && out_cap) snprintf(out_label, (size_t)out_cap, "%s", words[hit].label);
            return WAKE_CLOUD_REASSIGN;
        }
        return WAKE_CLOUD_DISCARD;
    }
    if (winner_i < 0 || words[winner_i].nsamples <= 0) return WAKE_CLOUD_LOCAL;
    return WAKE_CLOUD_DISCARD;
}

int wake_path_mtime(const char *path, long long *mtime_sec) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    if (mtime_sec) *mtime_sec = (long long)st.st_mtime;
    return 0;
}

int wake_native_snapshot(const char *path, unsigned *serial, long long *mtime_sec, char *kind, int kind_cap) {
    FILE *file;
    char line[80], word[32];
    unsigned value = 0, event = 0;
    if (serial) *serial = 0;
    if (mtime_sec) *mtime_sec = 0;
    if (kind && kind_cap) kind[0] = 0;
    if (wake_path_mtime(path, mtime_sec) != 0) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    if (!fgets(line, sizeof(line), file)) { fclose(file); return -1; }
    fclose(file);
    if (sscanf(line, "%31s %u %u", word, &value, &event) < 2) return -1;
    if (serial) *serial = value;
    if (kind && kind_cap) snprintf(kind, (size_t)kind_cap, "%s", word);
    return 0;
}

int wake_native_changed(unsigned before_serial, long long before_mtime,
                        unsigned now_serial, long long now_mtime) {
    return before_serial != now_serial || before_mtime != now_mtime;
}

int wake_cloud_pump(struct wake_cloud_pump *pump, int budget_ms, char *text, int cap) {
    int cancelled = 0;
    if (!pump || !pump->poll_text || budget_ms <= 0 || !text || cap < 2) return -1;
    text[0] = 0;
    if (pump->step_ms <= 0) pump->step_ms = 20;
    pump->elapsed_ms = 0;
    while (pump->elapsed_ms <= budget_ms) {
        int rc;
        if (pump->official_edge && pump->official_edge(pump->ctx)) {
            cancelled = 1;
            break;
        }
        rc = pump->poll_text(pump->ctx, text, cap);
        if (rc == 1) return text[0] ? 1 : 0;
        if (rc < 0) return -1;
        if (pump->elapsed_ms == budget_ms) break;
        pump->elapsed_ms += pump->step_ms;
        if (pump->elapsed_ms > budget_ms) pump->elapsed_ms = budget_ms;
    }
    if (cancelled) {
        if (pump->cancel) pump->cancel(pump->ctx);
        text[0] = 0;
        return -3;
    }
    text[0] = 0;
    return -2;
}
