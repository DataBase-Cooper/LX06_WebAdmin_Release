#include "home_mcp.h"
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char response[2048];

int xz_ws_send_text(xz_ws *ws, const char *text) {
    (void)ws;
    assert(strlen(text) < sizeof(response));
    strcpy(response, text);
    return 0;
}

static void call(xz_home_mcp *state, xz_ws *ws, const char *session, const char *payload,
                 uint64_t now, int closing) {
    char message[2048];
    assert(!xz_protocol_wrap_mcp(message, sizeof(message), session, payload));
    response[0] = '\0';
    assert(!xz_home_mcp_request(state, ws, "test-turn", message, strlen(message), now, closing));
}

static void await_reply(xz_home_mcp *state, xz_ws *ws, uint64_t now) {
    for (unsigned tick = 0; tick < 100 && state->pid; ++tick) {
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
        assert(!xz_home_mcp_poll(state, ws, "test-turn", now + tick * 10));
    }
    assert(!state->pid);
}

#define RPC(id, method) "{\"jsonrpc\":\"2.0\",\"id\":" id ",\"method\":\"" method "\""
#define ACTION(id, device, action) RPC(id, "tools/call") ",\"params\":{\"name\":\"self.smart_home.control\"," \
    "\"arguments\":{\"device\":\"" device "\",\"action\":\"" action "\"}}}"

int main(void) {
    char tmp[] = "/tmp/lx06-home-mcp-test.XXXXXX";
    assert(mkdtemp(tmp) && !chdir(tmp));
    FILE *f = fopen("mijia_mcp.sh", "w");
    assert(f);
    fputs("#!/bin/sh\n# LX06-HOME-MCP: 1\n"
          "[ \"$1\" = --home-mcp-action ] || exit 2\n"
          "[ -z \"${XZ_HOME_MCP_SCRIPT:-}\" ] || exit 2\n"
          "case \"$3\" in\n"
          "  客厅灯) printf '{\"code\":0}\\n';;\n"
          "  次卧灯) printf '{\"code\":-1}\\n';;\n"
          "  书房灯) exec /bin/sleep 8;;\n"
          "  客厅窗帘) printf '%s\\n' \"$2\" >> curtain-count; printf '{\"code\":0}\\n';;\n"
          "  *) exit 2;;\n"
          "esac\n", f);
    assert(!fclose(f));
    assert(!symlink("mijia_mcp.sh", "link.sh"));
    xz_ws ws = {.fd = -1};
    xz_home_mcp state = {0};
    xz_home_mcp_configure(&state, "../mijia_mcp.sh");
    xz_home_mcp_configure(&state, "link.sh");
    assert(!state.provider);
    call(&state, &ws, "test-turn", RPC("1", "tools/list") "}", 0, 0);
    assert(!response[0] && !state.pid);

    xz_home_mcp_configure(&state, "mijia_mcp.sh");
    assert(state.provider);
    call(&state, &ws, "test-turn", RPC("1", "initialize") "}", 0, 0);
    assert(strstr(response, "2024-11-05"));
    call(&state, &ws, "test-turn", RPC("2", "tools/list") "}", 0, 0);
    assert(strstr(response, "self.smart_home.control") && !strstr(response, "mijia_mcp.sh"));

    call(&state, &ws, "wrong-turn", ACTION("3", "客厅灯", "on"), 1, 0);
    assert(!response[0] && !state.pid);
    call(&state, &ws, "test-turn", ACTION("4", "客厅灯然后打开房门", "on"), 1, 0);
    assert(strstr(response, "-32602") && !state.pid);
    call(&state, &ws, "test-turn", ACTION("5", "客厅灯", "on"), 1, 1);
    assert(strstr(response, "-32000") && !state.pid);
    call(&state, &ws, "test-turn", ACTION("\"request-6\"", "客厅灯", "on"), 1, 0);
    assert(!response[0] && state.pid > 0);
    call(&state, &ws, "test-turn", ACTION("7", "次卧灯", "on"), 2, 0);
    assert(strstr(response, "-32000"));
    await_reply(&state, &ws, 2);
    assert(strstr(response, "\"request-6\"") && strstr(response, "\"isError\":false"));

    call(&state, &ws, "test-turn", ACTION("8", "次卧灯", "on"), 2000, 0);
    await_reply(&state, &ws, 2000);
    assert(strstr(response, "\"isError\":true"));
    call(&state, &ws, "test-turn", ACTION("9", "书房灯", "on"), 4000, 0);
    assert(state.pid > 0 && !xz_home_mcp_poll(&state, &ws, "test-turn", 9000));
    assert(!state.pid && strstr(response, "\"isError\":true"));
    assert(!xz_home_mcp_voice(&state, &ws, "如何关闭客厅窗帘？", 10000));
    assert(!xz_home_mcp_voice(&state, &ws, "打开客厅窗帘然后关闭客厅灯", 10001));
    assert(xz_home_mcp_voice(&state, &ws, "请关上客厅窗帘。", 10002) == 1);
    call(&state, &ws, "test-turn", ACTION("10", "客厅窗帘", "off"), 10003, 0);
    assert(state.pid && !response[0]);
    await_reply(&state, &ws, 10004);
    assert(strstr(response, "\"id\":10") && strstr(response, "\"isError\":false"));
    call(&state, &ws, "test-turn", ACTION("11", "客厅窗帘", "off"), 13000, 0);
    assert(!state.pid && strstr(response, "\"isError\":false"));
    assert(xz_home_mcp_voice(&state, &ws, "把客厅窗帘拉开。", 14000) == 1);
    await_reply(&state, &ws, 14001);
    f = fopen("curtain-count", "r");
    char count[32];
    assert(f && fgets(count, sizeof(count), f) && !strcmp(count, "off\n"));
    assert(fgets(count, sizeof(count), f) && !strcmp(count, "on\n"));
    assert(!fgets(count, sizeof(count), f) && !fclose(f) && !unlink("curtain-count"));
    assert(!unlink("mijia_mcp.sh"));
    call(&state, &ws, "test-turn", ACTION("12", "客厅灯", "on"), 18000, 0);
    assert(!state.pid && strstr(response, "-32001"));
    assert(!unlink("link.sh") && !chdir("/") && !rmdir(tmp));
    return 0;
}
