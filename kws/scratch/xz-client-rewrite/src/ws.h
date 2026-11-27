#ifndef XZ_WS_H
#define XZ_WS_H

#include <stddef.h>
#include <stdint.h>

#define XZ_WS_TEXT 1
#define XZ_WS_BINARY 2
#define XZ_WS_CLOSED 3

#define XZ_WS_MAX_MESSAGE (64u * 1024u)

typedef struct {
    int fd;
    int tls;
    int connected;
    void *ssl_ctx;
    void *ssl;
    char host[256];
    char path[1024];
    char port[8];
} xz_ws;

int xz_ws_connect(xz_ws *ws, const char *url, const char *device_id,
                  const char *client_id, const char *access_token, int timeout_ms);
int xz_ws_send_text(xz_ws *ws, const char *text);
int xz_ws_send_binary(xz_ws *ws, const unsigned char *data, size_t length);
int xz_ws_send_control(xz_ws *ws, int opcode, const unsigned char *data, size_t length);
int xz_ws_receive(xz_ws *ws, int *type, unsigned char *out, size_t capacity,
                  size_t *length, int timeout_ms);
int xz_ws_readable(xz_ws *ws, int timeout_ms);
void xz_ws_close(xz_ws *ws);

#endif
