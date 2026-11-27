#ifndef XZ_SESSION_H
#define XZ_SESSION_H

#include <stddef.h>

typedef enum {
    XZ_DISCONNECTED = 0,
    XZ_WAIT_HELLO,
    XZ_LISTENING,
    XZ_WAIT_RESPONSE,
    XZ_SPEAKING,
    XZ_CLOSING
} xz_state;

typedef struct {
    int version;
    int sample_rate;
    int channels;
    int frame_duration_ms;
    const char *transport;
    const char *format;
    const char *session_id; /* optional in the Feishu v1 service */
} xz_hello;

typedef struct {
    xz_state state;
    int protocol_error;
    int server_sample_rate;
    char session_id[96];
} xz_session;

typedef struct {
    unsigned required_frames;
    unsigned min_rms;
    unsigned consecutive;
    float max_echo_correlation;
} xz_barge_gate;

void xz_session_init(xz_session *s);
void xz_session_connected(xz_session *s);
int xz_session_accept_hello(xz_session *s, const xz_hello *hello);
int xz_session_listen_stop(xz_session *s);
int xz_session_tts_start(xz_session *s);
int xz_session_tts_stop(xz_session *s);
int xz_session_abort(xz_session *s);

void xz_barge_gate_init(xz_barge_gate *g, unsigned required_frames, unsigned min_rms);
void xz_barge_gate_reset(xz_barge_gate *g);
int xz_barge_gate_observe(xz_barge_gate *g, int tts_active, unsigned residual_rms,
                          float echo_correlation, int speech_like);

#endif
