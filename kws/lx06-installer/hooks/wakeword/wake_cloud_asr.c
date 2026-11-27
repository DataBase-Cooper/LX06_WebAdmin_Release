/* One-shot helper for the existing control-asr probe socket.
 *
 * The cloud connection and the device's official credentials stay inside
 * hooks/xiaoai/control-asr-probe.cc (LX06_CONTROL_ASR_PROBE=1, root only,
 * reuses the official Engine). This program does not embed a token or open
 * a second cloud link.
 *
 * Packet layout matches control-asr-probe-client.c framed mode and
 * runtime/common/control_asr.c: one-byte 'S', then 'A' + 2-byte big-endian
 * Opus length + Opus, then 'E' (RecognizeStreamFinished). 'C' cancels.
 * 'S' is the probe client's non-VAD start: this clip is already finished, so
 * the helper sends E itself. lx06_control_asr_feed() cannot do that inside
 * 800ms; its poll waits 1600ms before emitting E, and feed(protect=0) sends
 * C, which cancels. Live 20ms pacing from the probe client is omitted because
 * that pacing measured 0/68 within 800ms.
 *
 * Stdin is s16le 16 kHz mono. Stdout is one line of final text, or empty.
 * Exit 0 = final, 2 = probe/socket/codec unavailable, 3 = cancelled, 4 = budget.
 *
 * Optional argv: socket [budget_ms] [max_samples]. Defaults stay 800ms and
 * 1.8s for the wake path, which passes only the socket. Enrollment passes
 * 2500 and 40000; that does not change the wake-time call.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <opus/opus.h>
#include "../../runtime/doubao/src/json.h"

#define RATE 16000
#define FRAME 320
#define DEFAULT_MAX_SAMPLES 28800
#define ABS_MAX_SAMPLES 40000
#define DEFAULT_BUDGET_MS 800
static volatile int asr_fd = -1;

static void stop(int sig) {
    (void)sig;
    int fd = asr_fd;
    if (fd >= 0) {
        unsigned char cancel = 'C';
        /* Cancellation must never wait behind a full probe send queue. */
        (void)send(fd, &cancel, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    _exit(3);
}

static int elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

static int send_all(int fd, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* 1 = message, 0 = timeout, -1 = socket error. */
static int recv_msg(int fd, char *buf, size_t cap, int wait_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, wait_ms);
    if (pr == 0) return 0;
    if (pr < 0) return errno == EINTR ? 0 : -1;
    ssize_t n = recv(fd, buf, cap - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    return 1;
}

static int message_type(const char *msg, char *type, size_t cap) {
    xz_json_doc doc;
    if (xz_json_parse(&doc, msg, strlen(msg))) return -1;
    return xz_json_get_string(&doc, doc.root, "type", type, cap);
}

static int ready_dialog(const char *msg, char *dialog, size_t cap) {
    xz_json_doc doc;
    char type[32];
    if (xz_json_parse(&doc, msg, strlen(msg))) return -1;
    if (xz_json_get_string(&doc, doc.root, "type", type, sizeof(type))) return 0;
    if (strcmp(type, "ready")) return 0;
    return xz_json_get_string(&doc, doc.root, "dialog", dialog, cap) == 0;
}

static int final_text(const char *msg, const char *dialog, char *text, size_t cap) {
    xz_json_doc doc;
    char id[96], ns[64], name[64];
    int header, body, final, results;
    if (xz_json_parse(&doc, msg, strlen(msg))) return 0;
    header = xz_json_object_get(&doc, doc.root, "header");
    body = xz_json_object_get(&doc, doc.root, "payload");
    if (xz_json_get_string(&doc, header, "dialog_id", id, sizeof(id)) || strcmp(id, dialog) ||
        xz_json_get_string(&doc, header, "namespace", ns, sizeof(ns)) ||
        xz_json_get_string(&doc, header, "name", name, sizeof(name))) return 0;
    if (strcmp(ns, "SpeechRecognizer") || strcmp(name, "RecognizeResult")) return 0;
    final = xz_json_object_get(&doc, body, "is_final");
    results = xz_json_object_get(&doc, body, "results");
    if (final < 0 || doc.tokens[final].end - doc.tokens[final].start != 4 ||
        memcmp(msg + doc.tokens[final].start, "true", 4) ||
        results < 0 || doc.tokens[results].type != XZ_JSON_ARRAY) return 0;
    if (xz_json_get_string(&doc, doc.tokens[results].first_child, "text", text, cap)) text[0] = 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/lx06-control-asr-probe.sock";
    int16_t pcm[ABS_MAX_SAMPLES];
    int samples = 0, error = 0, fd, at;
    int budget_ms = DEFAULT_BUDGET_MS, max_samples = DEFAULT_MAX_SAMPLES;
    OpusEncoder *encoder;
    struct sockaddr_un addr;
    struct timespec start;
    struct sigaction action;
    char dialog[96] = {0}, message[8192], text[512];
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    if (argc > 2) {
        budget_ms = atoi(argv[2]);
        if (budget_ms < 200 || budget_ms > 4000) return 2;
    }
    if (argc > 3) {
        max_samples = atoi(argv[3]);
        if (max_samples < 8000 || max_samples > ABS_MAX_SAMPLES) return 2;
    }
    size_t pcm_bytes = 0;
    while (pcm_bytes < (size_t)max_samples * 2) {
        ssize_t n = read(STDIN_FILENO, (unsigned char *)pcm + pcm_bytes,
                         (size_t)max_samples * 2 - pcm_bytes);
        if (n == 0) break;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return 2;
        pcm_bytes += (size_t)n;
    }
    if (pcm_bytes % 2) return 2;
    samples = (int)(pcm_bytes / 2);
    if (samples == max_samples) {
        unsigned char extra;
        ssize_t n = read(STDIN_FILENO, &extra, 1);
        if (n > 0) return 2;
        if (n < 0 && errno != EINTR) return 2;
    }
    if (samples < 8000 || samples > max_samples) return 2;
    encoder = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error) return 2;
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(3));
    if (strlen(path) >= sizeof(addr.sun_path)) return 2;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return 2;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (connect(fd, (void *)&addr, sizeof(addr)) != 0) return 2;
    asr_fd = fd;
    /* Probe sends hello before it reads. Ignore it; a missing hello still
     * continues, because a later ready/error tells us whether start worked. */
    if (recv_msg(fd, message, sizeof(message), 50) < 0) return 2;
    /* 'S' = probe-client default: asr.vad false, we close the clip with E. */
    if (send_all(fd, "S", 1) != 0) return 2;
    while (!dialog[0]) {
        int left = budget_ms - elapsed_ms(&start);
        int rc;
        if (left <= 0) return 4;
        rc = recv_msg(fd, message, sizeof(message), left > 100 ? 100 : left);
        if (rc < 0) return 2;
        if (rc == 0) continue;
        if (strstr(message, "\"error\"")) return 2;
        ready_dialog(message, dialog, sizeof(dialog));
    }
    /* Wake-time supervision needs the exact control-ASR ID before any PCM
     * reaches the probe. Enrollment keeps its historical single text line. */
    const char *report_dialog=getenv("LX06_WAKE_REPORT_DIALOG");
    if(report_dialog && !strcmp(report_dialog,"1") && !strncmp(dialog,"lx06-ctrl-",10)) {
        if(strpbrk(dialog,"\r\n") || fprintf(stdout,"\036%s\n",dialog)<0 || fflush(stdout)) return 2;
    }
    for (at = 0; at + FRAME <= samples; at += FRAME) {
        unsigned char packet[4000];
        int bytes = opus_encode(encoder, pcm + at, FRAME, packet + 3, (int)sizeof(packet) - 3);
        if (bytes <= 0) return 2;
        packet[0] = 'A';
        packet[1] = (unsigned char)(bytes >> 8);
        packet[2] = (unsigned char)bytes;
        if (send_all(fd, packet, (size_t)bytes + 3) != 0) return 2;
    }
    opus_encoder_destroy(encoder);
    if (elapsed_ms(&start) >= budget_ms) return 4;
    if (send_all(fd, "E", 1) != 0) return 2;
    while (elapsed_ms(&start) < budget_ms) {
        int left = budget_ms - elapsed_ms(&start);
        int rc = recv_msg(fd, message, sizeof(message), left > 20 ? 20 : left);
        if (rc < 0) return 2;
        if (rc == 0) continue;
        if (final_text(message, dialog, text, sizeof(text))) {
            printf("%s\n", text);
            return 0;
        }
    }
    if (send_all(fd, "C", 1) != 0) return 4;
    return 4;
}
