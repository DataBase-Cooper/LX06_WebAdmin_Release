#include "../src/ws.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    xz_ws ws;
    if (xz_ws_connect(&ws, argv[1], "02:00:00:00:00:01",
                      "00000000-0000-4000-8000-000000000001", "test-token", 3000) != 0) {
        fputs("connect failed\n", stderr);
        return 3;
    }
    if (xz_ws_send_text(&ws, "hello-client") != 0) {
        xz_ws_close(&ws);
        return 4;
    }
    unsigned char response[64];
    size_t length = 0;
    int type = 0;
    int rc = xz_ws_receive(&ws, &type, response, sizeof(response), &length, 3000);
    xz_ws_close(&ws);
    if (rc != 1 || type != XZ_WS_TEXT || length != strlen("hello-server") ||
        memcmp(response, "hello-server", length) != 0) return 5;
    puts("ws integration client: PASS");
    return 0;
}
