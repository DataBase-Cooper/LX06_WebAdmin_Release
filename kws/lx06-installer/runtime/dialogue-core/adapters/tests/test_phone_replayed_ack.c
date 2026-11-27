/* Offline integration: production remote packet dispatcher, ACK parser and
 * actual local SEQPACKET delivery. No transport listener or device is opened. */
#define _GNU_SOURCE
#ifdef NDEBUG
#error assertions must be enabled
#endif
#include <assert.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include "../phone.c"

static void inbound(struct phone *w, unsigned type, uint64_t epoch, const char *json) {
    dc_wire_packet p = {0};
    size_t n = strlen(json);
    p.header = dc_wire_make(type, epoch, 0, 0, n);
    memcpy(p.payload, json, n);
    int r = remote_packet(w, (const unsigned char *)&p, sizeof p.header + n);
    assert(r == 0);
}
static int drain(int fd, int *ready) {
    int count = 0;
    dc_wire_packet p;
    while (dc_wire_receive(fd, &p) == 1) {
        assert(p.header.epoch == 123);
        if (p.header.type == DC_EVT_READY) ++*ready;
        ++count;
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    return count;
}
static const char *fresh = "{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"123\",\"command_id\":\"command_123\",\"status\":\"completed\",\"code\":\"ok\",\"replayed\":false}";
static const char *replay = "{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"123\",\"command_id\":\"command_123\",\"status\":\"completed\",\"code\":\"ok\",\"replayed\":true}";
static const char *status = "{\"probe_only\":false,\"call_ready\":true,\"capabilities\":{\"virtual_mic\":true,\"rtc_capture\":true,\"call_lifecycle\":true,\"reply_lifecycle\":true,\"turn_fence\":false,\"automatic_ui\":false}}";
static void scenario(int replay_first) {
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv);
    assert(r == 0);
    struct phone w = {0};
    w.core = sv[0]; w.socket = -1; w.epoch = 123;
    strcpy(w.command, "command_123");
    inbound(&w, DC_EVT_TEXT, 124, fresh); /* old session ignored */
    inbound(&w, DC_EVT_TEXT, 123, replay_first ? replay : fresh);
    inbound(&w, DC_EVT_TEXT, 123, replay_first ? fresh : replay);
    inbound(&w, DC_EVT_READY, 123, status);
    r = local_flush(&w); assert(r == 0);
    int ready = 0, count = drain(sv[1], &ready);
    assert(count == (replay_first ? 3 : 4));
    assert(ready == (replay_first ? 0 : 1));
    close(sv[0]); close(sv[1]);
}
int main(void) {
    scenario(0);
    scenario(1);
    puts("production ACK/status dispatch and local delivery: PASS");
    return 0;
}
