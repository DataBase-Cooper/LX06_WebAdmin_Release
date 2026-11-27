#include "../greeting_request.h"
#include "../../model-adapter/experiments/native-tts-ipc.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "request check failed line %d\n", __LINE__); return 1; } } while (0)

/* A local provider stub sends the actual v1 READY response, not a fictional
 * identity field. Socketpair traffic is offline, never reaches the device. */
int main(void) {
    int pair[2]; unsigned char frame[NTTS_HEADER_BYTES];
    dc_greeting_request q = {0};
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    CHECK(dc_greeting_request_submit(&q, DC_GREETING_UPLOAD, (const unsigned char *)"hello", 5) == 0);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_BLOCKED_IDENTITY);
    CHECK(dc_greeting_request_submit(&q, DC_GREETING_REGENERATE, (const unsigned char *)"edited", 6) != 0);
    struct ntts_header h = {NTTS_WARM, 0, 1, 0, 0}, incoming;
    ntts_encode(frame, &h);
    CHECK(send(pair[1], frame, sizeof frame, 0) == (ssize_t)sizeof frame);
    CHECK(recv(pair[0], frame, sizeof frame, 0) == (ssize_t)sizeof frame);
    CHECK(ntts_decode(frame, sizeof frame, &incoming) == 0);
    CHECK(incoming.type == NTTS_WARM && !incoming.request_id && !incoming.payload_len);
    h.type = NTTS_READY; h.request_id = 1;
    ntts_encode(frame, &h);
    CHECK(send(pair[0], frame, sizeof frame, 0) == (ssize_t)sizeof frame);
    CHECK(recv(pair[1], frame, sizeof frame, 0) == (ssize_t)sizeof frame);
    CHECK(dc_greeting_request_observe_ready(&q, frame, sizeof frame) == 0);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_BLOCKED_IDENTITY);
    CHECK(q.text_len == 5 && !memcmp(q.text, "hello", 5));
    frame[0] = 0;
    CHECK(dc_greeting_request_observe_ready(&q, frame, sizeof frame) != 0);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_BLOCKED_IDENTITY);
    dc_greeting_request_cancel(&q);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_EMPTY);
    CHECK(q.text_len == 0 && q.text[0] == 0);
    CHECK(dc_greeting_request_submit(&q, DC_GREETING_REGENERATE, (const unsigned char *)"edited", 6) == 0);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_BLOCKED_IDENTITY);
    dc_greeting_request_cancel(&q);
    CHECK(dc_greeting_request_submit(&q, DC_GREETING_UPLOAD, (const unsigned char *)"x\0y", 3) != 0);
    CHECK(dc_greeting_request_status(&q) == DC_GREETING_EMPTY);
    close(pair[0]); close(pair[1]);
    puts("greeting request fail-closed: PASS");
    return 0;
}
