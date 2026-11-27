#include "../src/session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_hello_must_be_validated_before_audio(void) {
    xz_session session;
    xz_session_init(&session);
    assert(session.state == XZ_DISCONNECTED);

    xz_session_connected(&session);
    assert(session.state == XZ_WAIT_HELLO);

    xz_hello good = {1, 16000, 1, 60, "websocket", "opus", NULL};
    assert(xz_session_accept_hello(&session, &good) == 0);
    assert(session.state == XZ_LISTENING);
    assert(session.server_sample_rate == 16000);

    xz_session_init(&session);
    xz_session_connected(&session);
    xz_hello server_24khz = {1, 24000, 1, 60, "websocket", "opus", NULL};
    assert(xz_session_accept_hello(&session, &server_24khz) == 0);
    assert(session.state == XZ_LISTENING);
    assert(session.server_sample_rate == 24000);

    xz_session_init(&session);
    xz_session_connected(&session);
    xz_hello wrong_rate = {1, 48000, 1, 60, "websocket", "opus", NULL};
    assert(xz_session_accept_hello(&session, &wrong_rate) != 0);
    assert(session.state == XZ_WAIT_HELLO);
}

static void test_tts_start_stop_and_abort_restore_listening(void) {
    xz_session session;
    xz_session_init(&session);
    xz_session_connected(&session);
    xz_hello hello = {1, 16000, 1, 60, "websocket", "opus", NULL};
    assert(xz_session_accept_hello(&session, &hello) == 0);

    assert(xz_session_tts_start(&session) == 0);
    assert(session.state == XZ_SPEAKING);
    assert(xz_session_tts_stop(&session) == 0);
    assert(session.state == XZ_LISTENING);

    assert(xz_session_tts_start(&session) == 0);
    assert(xz_session_abort(&session) == 0);
    assert(session.state == XZ_LISTENING);
}

static void test_barge_gate_rejects_echo_and_requires_stable_voice(void) {
    xz_barge_gate gate;
    xz_barge_gate_init(&gate, 3, 2500);

    /* High microphone level correlated with the speaker is the client's echo. */
    for (int i = 0; i < 5; ++i)
        assert(!xz_barge_gate_observe(&gate, 1, 3200, 0.92f, 1));

    /* One transient cannot interrupt; three consecutive uncorrelated speech frames can. */
    assert(!xz_barge_gate_observe(&gate, 1, 3200, 0.08f, 1));
    assert(!xz_barge_gate_observe(&gate, 1, 3200, 0.08f, 1));
    assert(xz_barge_gate_observe(&gate, 1, 3200, 0.08f, 1));

    /* A non-voice TV/noise burst is not enough even when loud and uncorrelated. */
    xz_barge_gate_reset(&gate);
    for (int i = 0; i < 6; ++i)
        assert(!xz_barge_gate_observe(&gate, 1, 6000, 0.05f, 0));
}

int main(void) {
    test_hello_must_be_validated_before_audio();
    test_tts_start_stop_and_abort_restore_listening();
    test_barge_gate_rejects_echo_and_requires_stable_voice();
    puts("session tests: PASS");
    return 0;
}
