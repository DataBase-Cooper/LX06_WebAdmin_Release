#ifndef DC_GREETING_REQUEST_H
#define DC_GREETING_REQUEST_H
#include <stddef.h>

/* One in-memory request, owned and serialized by the caller. This interface has
 * no synthesis or cache-consumption operation: READY is not voice identity. */
#define DC_GREETING_TEXT_MAX 240

enum dc_greeting_reason { DC_GREETING_UPLOAD = 1, DC_GREETING_REGENERATE = 2 };
enum dc_greeting_status { DC_GREETING_EMPTY = 0, DC_GREETING_BLOCKED_IDENTITY = 1 };
typedef struct {
    unsigned char text[DC_GREETING_TEXT_MAX];
    size_t text_len;
    enum dc_greeting_reason reason;
} dc_greeting_request;

/* Initialize to zero. A full slot rejects rather than discarding a request.
 * Success means queued in volatile memory only, never generated or persisted. */
int dc_greeting_request_submit(dc_greeting_request *q, enum dc_greeting_reason reason,
                               const unsigned char *text, size_t len);
enum dc_greeting_status dc_greeting_request_status(const dc_greeting_request *q);
/* Only validates the v1 READY envelope; it cannot establish voice identity.
 * Caller must separately authenticate transport before considering any frame. */
int dc_greeting_request_observe_ready(dc_greeting_request *q, const unsigned char *packet, size_t len);
void dc_greeting_request_cancel(dc_greeting_request *q);
#endif
