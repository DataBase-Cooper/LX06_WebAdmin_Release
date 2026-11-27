#include "greeting_request.h"
#include "../model-adapter/experiments/native-tts-ipc.h"
#include <string.h>

int dc_greeting_request_submit(dc_greeting_request *q, enum dc_greeting_reason reason,
                               const unsigned char *text, size_t len) {
    if (!q || q->text_len || (reason != DC_GREETING_UPLOAD && reason != DC_GREETING_REGENERATE) ||
        !text || len > DC_GREETING_TEXT_MAX || !ntts_text_valid(text, len)) return -1;
    memcpy(q->text, text, len);
    q->text_len = len;
    q->reason = reason;
    return 0;
}

enum dc_greeting_status dc_greeting_request_status(const dc_greeting_request *q) {
    return q && q->text_len ? DC_GREETING_BLOCKED_IDENTITY : DC_GREETING_EMPTY;
}

int dc_greeting_request_observe_ready(dc_greeting_request *q, const unsigned char *packet, size_t len) {
    struct ntts_header h;
    if (!q || !packet || ntts_decode(packet, len, &h) || h.type != NTTS_READY ||
        !h.request_id || !h.generation || h.payload_len || h.code) return -1;
    return 0;
}

void dc_greeting_request_cancel(dc_greeting_request *q) {
    if (!q) return;
    volatile unsigned char *p = (volatile unsigned char *)q;
    for (size_t i = 0; i < sizeof *q; ++i) p[i] = 0;
}
