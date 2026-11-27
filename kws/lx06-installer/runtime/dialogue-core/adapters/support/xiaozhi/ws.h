#ifndef DC_XZ_WS_H
#define DC_XZ_WS_H

#include <stddef.h>
#include <stdint.h>

#define DC_XZ_WS_TEXT 1
#define DC_XZ_WS_BINARY 2
#define DC_XZ_WS_CLOSED 3

#define DC_XZ_WS_MAX_MESSAGE (64u * 1024u)

typedef struct {
    int fd;
    int tls;
    int connected;
    void *ssl_ctx;
    void *ssl;
    char host[256];
    char path[1024];
    char port[8];
    /* Last exact8-byte PONG token, for the transport's single pending ping. */
    uint64_t pong_token;
} dc_xz_ws;

int dc_xz_ws_connect(dc_xz_ws *ws, const char *url, const char *device_id,
                  const char *client_id, const char *access_token, int timeout_ms);
int dc_xz_ws_send_text(dc_xz_ws *ws, const char *text);
int dc_xz_ws_send_binary(dc_xz_ws *ws, const unsigned char *data, size_t length);
int dc_xz_ws_send_control(dc_xz_ws *ws, int opcode, const unsigned char *data, size_t length);
int dc_xz_ws_receive(dc_xz_ws *ws, int *type, unsigned char *out, size_t capacity,
                  size_t *length, int timeout_ms);
int dc_xz_ws_readable(dc_xz_ws *ws, int timeout_ms);
void dc_xz_ws_close(dc_xz_ws *ws);

#endif
