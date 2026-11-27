#include "session.h"

#include <string.h>

void xz_session_init(xz_session *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = XZ_DISCONNECTED;
}

void xz_session_connected(xz_session *s) {
    if (!s) return;
    s->state = XZ_WAIT_HELLO;
    s->protocol_error = 0;
    s->server_sample_rate = 0;
    s->session_id[0] = '\0';
}

int xz_session_accept_hello(xz_session *s, const xz_hello *h) {
    if (!s || !h || s->state != XZ_WAIT_HELLO) return -1;
    if (h->version != 1 || (h->sample_rate != 16000 && h->sample_rate != 24000) ||
        h->channels != 1 || h->frame_duration_ms != 60 || !h->transport ||
        strcmp(h->transport, "websocket") != 0 || !h->format ||
        strcmp(h->format, "opus") != 0) {
        s->protocol_error = 1;
        return -1;
    }
    s->server_sample_rate = h->sample_rate;
    if (h->session_id) {
        size_t n = strlen(h->session_id);
        if (n >= sizeof(s->session_id)) {
            s->protocol_error = 1;
            return -1;
        }
        memcpy(s->session_id, h->session_id, n + 1);
    }
    s->protocol_error = 0;
    s->state = XZ_LISTENING;
    return 0;
}

int xz_session_listen_stop(xz_session *s) {
    if (!s || s->state != XZ_LISTENING) return -1;
    s->state = XZ_WAIT_RESPONSE;
    return 0;
}

int xz_session_tts_start(xz_session *s) {
    if (!s || (s->state != XZ_LISTENING && s->state != XZ_WAIT_RESPONSE)) return -1;
    s->state = XZ_SPEAKING;
    return 0;
}

int xz_session_tts_stop(xz_session *s) {
    if (!s || s->state != XZ_SPEAKING) return -1;
    s->state = XZ_LISTENING;
    return 0;
}

int xz_session_abort(xz_session *s) {
    if (!s || s->state != XZ_SPEAKING) return -1;
    s->state = XZ_LISTENING;
    return 0;
}

void xz_barge_gate_init(xz_barge_gate *g, unsigned required_frames, unsigned min_rms) {
    if (!g) return;
    g->required_frames = required_frames ? required_frames : 1;
    g->min_rms = min_rms;
    g->consecutive = 0;
    g->max_echo_correlation = 0.55f;
}

void xz_barge_gate_reset(xz_barge_gate *g) {
    if (g) g->consecutive = 0;
}

int xz_barge_gate_observe(xz_barge_gate *g, int tts_active, unsigned residual_rms,
                          float echo_correlation, int speech_like) {
    if (!g) return 0;
    if (!tts_active || !speech_like || residual_rms < g->min_rms ||
        echo_correlation >= g->max_echo_correlation) {
        g->consecutive = 0;
        return 0;
    }
    if (++g->consecutive >= g->required_frames) {
        g->consecutive = 0;
        return 1;
    }
    return 0;
}
