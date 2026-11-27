/* Listen-window helper. webd forks this only after a wake, and only while the
 * window is open. It attaches one control_asr WINDOW reader and one pcmtee
 * tap, then drops both before exit. It does not open a second microphone. */
#define _GNU_SOURCE
#include "listen-window.h"
#include "channel-route.h"
#include "official-bind.h"
#include "../../runtime/common/control_asr.h"
#include <opus/opus.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef LX06_LISTEN_RESULT
#define LX06_LISTEN_RESULT "/tmp/lx06-listen-result"
#endif
#ifndef LX06_LISTEN_TAP
#define LX06_LISTEN_TAP "/tmp/xiaoai-agent/taps/lx06-listen.usock"
#endif
#ifndef LX06_NATIVE_EVENT
#define LX06_NATIVE_EVENT "/tmp/lx06-native-wake"
#endif
#define LX06_ASR_SOCK "/tmp/lx06-control-asr-probe.sock"

static lx06_control_asr *asr;
static int asr_failed;
static unsigned start_ms = 5000, max_ms = 10000;

static int asr_open(void *ctx) {
    (void)ctx;
    asr = lx06_control_asr_open_profile(LX06_ASR_SOCK, LX06_ASR_WINDOW);
    if (!asr) { asr_failed = 1; return -1; }
    lx06_control_asr_window_timing(asr, start_ms, max_ms);
    return 0;
}
static void asr_close(void *ctx) {
    (void)ctx;
    if (asr) { lx06_control_asr_close(asr); asr = NULL; }
}
static int asr_feed(void *ctx, const void *pcm, size_t n, uint64_t now) {
    (void)ctx;
    if (!asr) return -1;
    return lx06_control_asr_feed(asr, 1, pcm, n, now);
}
static int asr_poll(void *ctx, char *buf, size_t cap, uint64_t now) {
    (void)ctx;
    if (!asr) return -1;
    return lx06_control_asr_poll(asr, buf, cap, now);
}
static int asr_readers(void *ctx) {
    (void)ctx;
    return lx06_control_asr_window_readers();
}
static int asr_speech_seen(void *ctx, uint64_t *started) {
    (void)ctx;
    return lx06_control_asr_speech_seen(asr, started);
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int load_script(const char *id, struct lx06_match_candidate *cands, int max,
                       int *count, unsigned *start, unsigned *limit) {
    char path[320], buf[8192];
    const char *key = "# LX06-LOCAL-CHANNEL:";
    const char *mskey = "# LX06-CHANNEL-WINDOW-MS:";
    const char *startkey = "# LX06-CHANNEL-WINDOW-START-MS:";
    const char *maxkey = "# LX06-CHANNEL-WINDOW-MAX-MS:";
    ssize_t n;
    int fd;
    char *line, *save;
    *count = 0;
    *start = 5000;
    *limit = 10000;
    if (!lx06_bind_name_ok(id)) return -1;
    snprintf(path, sizeof(path), "%s/%s", LX06_BIND_APPS, id);
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    {
        char *ms = strstr(buf, maxkey);
        char *first = strstr(buf, startkey);
        const char *key_used = maxkey;
        if (!ms) { ms = strstr(buf, mskey); key_used = mskey; }
        if (ms) *limit = (unsigned)lx06_window_ms_clamp((unsigned)atoi(ms + strlen(key_used)));
        if (first) *start = (unsigned)lx06_window_start_ms_clamp((unsigned)atoi(first + strlen(startkey)));
    }
    line = strtok_r(buf, "\n", &save);
    while (line && *count < max) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        if (len > strlen(key) && !strncmp(line, key, strlen(key))) {
            char *phrase = line + strlen(key);
            char *part, *psave;
            while (*phrase == ' ') phrase++;
            part = strtok_r(phrase, "|", &psave);
            while (part && *count < max) {
                while (*part == ' ') part++;
                if (*part) {
                    cands[*count].script = id;
                    cands[*count].line = *count;
                    cands[*count].phrase = strdup(part);
                    if (!cands[*count].phrase) return -1;
                    (*count)++;
                }
                part = strtok_r(NULL, "|", &psave);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return *count > 0 ? 0 : -1;
}

static int bind_tap(void) {
    struct sockaddr_un addr;
    int fd;
    mkdir("/tmp/xiaoai-agent", 0755);
    mkdir("/tmp/xiaoai-agent/taps", 0755);
    unlink(LX06_LISTEN_TAP);
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LX06_LISTEN_TAP);
    if (bind(fd, (void *)&addr, sizeof(addr)) != 0) { close(fd); unlink(LX06_LISTEN_TAP); return -1; }
    return fd;
}

static int official_since(time_t start) {
    struct stat st;
    char buf[32];
    int fd;
    ssize_t n;
    if (!lx06_official_kws_enabled()) return 0;
    if (stat(LX06_NATIVE_EVENT, &st) != 0 || st.st_mtime < start) return 0;
    fd = open(LX06_NATIVE_EVENT, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    return !strncmp(buf, "official ", 9);
}

static void write_result(const char *status, const char *script, const char *source, const char *text) {
    char tmp[640], line[512];
    int fd, n;
    snprintf(tmp, sizeof(tmp), "%s.tmp", LX06_LISTEN_RESULT);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    n = snprintf(line, sizeof(line), "status=%s script=%s source=%s text=%s\n",
                 status, script ? script : "-", source ? source : "local", text ? text : "-");
    if (n > 0) {
        ssize_t w = write(fd, line, (size_t)n);
        if (w != n) n = -1;
    }
    close(fd);
    if (n > 0) rename(tmp, LX06_LISTEN_RESULT);
    else unlink(tmp);
}

static const char *status_name(int status) {
    switch (status) {
    case LX06_LISTEN_HIT: return "hit";
    case LX06_LISTEN_TIMEOUT: return "timeout";
    case LX06_LISTEN_ASR_FAIL: return "asr";
    case LX06_LISTEN_MISS: return "miss";
    case LX06_LISTEN_REJECT: return "reject";
    default: return "cancel";
    }
}

/* No socket, no script, no log of speech. Proves the ARM binary's opus
 * encoder and the shared matcher both run. */
static int selftest(void) {
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    opus_int16 pcm[320];
    unsigned char packet[256];
    int bytes;
    struct lx06_match_candidate row = {"qishui", 5, "我要听{query}"};
    struct lx06_match_result mr;
    if (!enc || err) return 1;
    memset(pcm, 0, sizeof(pcm));
    bytes = opus_encode(enc, pcm, 320, packet, (opus_int32)sizeof(packet));
    opus_encoder_destroy(enc);
    if (bytes <= 1) return 1;
    if (lx06_template_match("我要听晴天", &row, 1, &mr) != 0) return 1;
    if (mr.kind != LX06_MATCH_TEMPLATE || mr.var_count != 1 || strcmp(mr.vars[0].value, "晴天")) return 1;
    if (lx06_control_asr_window_readers() != 0) return 1;
    if (lx06_window_ms_clamp(2000) != 3000 || lx06_window_ms_clamp(0) != 10000 ||
        lx06_window_start_ms_clamp(0) != 5000 || lx06_window_start_ms_clamp(500) != 1000 ||
        lx06_window_start_ms_clamp(20000) != 15000 || lx06_window_ms_clamp(20000) != 15000) return 1;
    puts("listen-window selftest passed");
    return 0;
}

int main(int argc, char **argv) {
    struct lx06_match_candidate cands[32];
    int count = 0, tap, i;
    struct lx06_listen_config cfg;
    struct lx06_listen_window *window;
    struct lx06_listen_outcome outcome;
    time_t wall = time(NULL);
    const char *script, *source;
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc != 3) return 2;
    script = argv[1];
    source = argv[2];
    if (strcmp(source, "local") && strcmp(source, "official")) return 2;
    if (load_script(script, cands, 32, &count, &start_ms, &max_ms) != 0) {
        write_result("miss", script, source, "-");
        return 1;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.cands = cands;
    cfg.cand_count = count;
    cfg.start_ms = start_ms;
    cfg.max_ms = max_ms;
    cfg.asr.open = asr_open;
    cfg.asr.close = asr_close;
    cfg.asr.feed = asr_feed;
    cfg.asr.poll = asr_poll;
    cfg.asr.readers = asr_readers;
    cfg.asr.speech_seen = asr_speech_seen;
    window = lx06_listen_open(&cfg, mono_ms());
    if (!window) {
        write_result("asr", script, source, "-");
        for (i = 0; i < count; i++) free((void *)cands[i].phrase);
        return 1;
    }
    tap = bind_tap();
    while (lx06_listen_poll(window, mono_ms(), &outcome) == 0) {
        unsigned char pcm[640];
        if (official_since(wall)) { lx06_listen_cancel(window, mono_ms()); break; }
        if (tap >= 0) {
            ssize_t n = recv(tap, pcm, sizeof(pcm), 0);
            if (n > 0) lx06_listen_feed(window, pcm, (size_t)n, mono_ms());
        }
        usleep(20000);
    }
    lx06_listen_poll(window, mono_ms(), &outcome);
    if (tap >= 0) { close(tap); unlink(LX06_LISTEN_TAP); }
    /* Normalized text has no spaces, so the result line stays one token.
     * stderr stays silent about the sentence. */
    write_result(status_name(outcome.status), script, source,
                 outcome.status == LX06_LISTEN_HIT && outcome.text[0] ? outcome.text : "-");
    lx06_listen_close(window);
    for (i = 0; i < count; i++) free((void *)cands[i].phrase);
    return asr_readers(NULL) == 0 ? 0 : 3;
}
