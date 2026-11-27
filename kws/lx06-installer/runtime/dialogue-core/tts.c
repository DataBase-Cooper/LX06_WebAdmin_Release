#define _GNU_SOURCE
#include "tts.h"
#include "../model-adapter/experiments/native-tts-ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define TTS_RETIRED_CAP 8u
#define TTS_URL_MS UINT64_C(15000)
#define TTS_DECODE_MS UINT64_C(45000)
#define TTS_CANCEL_MS UINT64_C(80)
#define TTS_STREAM_LENGTH UINT32_C(0x7ffff000)
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __lxstat64(int, const char *, struct stat64 *);
#define TTS_STATBUF struct stat64
#define TTS_LSTAT(path, out) __lxstat64(3, (path), (out))
#else
#define TTS_STATBUF struct stat
#define TTS_LSTAT(path, out) lstat((path), (out))
#endif

struct channel { int fd, state; size_t length; unsigned char command[NTTS_HEADER_BYTES + NTTS_MAX_TEXT]; };
struct tts_slot {
    int used, ready;
    dc_tag tag;
    uint64_t sequence, deadline;
    struct channel channel;
    size_t url_length, url_at;
    unsigned char url[NTTS_MAX_PAYLOAD];
};
struct tts_retired { pid_t pid; uint64_t kill_at; int killed; };
struct dc_tts {
    char socket_path[108], decoder_path[4096];
    pid_t broker_pid;
    struct channel warm;
    dc_tag warm_tag, last_tag;
    uint64_t warm_deadline, last_sequence;
    int invalidated;
    struct tts_slot slots[DC_TTS_SLOTS];
    struct tts_retired retired[TTS_RETIRED_CAP];
    int active, input_fd, output_fd, eof, header_valid, decoder_done;
    pid_t decoder_pid;
    uint64_t decoder_deadline;
    unsigned rate, channels;
    unsigned char header[44], frame[4];
    size_t header_used, frame_used, source_bytes, pcm_bytes;
    uint32_t declared_bytes;
    unsigned char pcm[DC_TTS_PCM_CAP], packet[NTTS_MAX_PACKET];
    size_t pcm_head, pcm_used;
};
static int same_reply(dc_tag a, dc_tag b) {
    return a.session_epoch == b.session_epoch && a.reply_generation == b.reply_generation;
}
static void wipe(void *memory, size_t size) { volatile unsigned char *p = memory; while (size--) *p++ = 0; }
static void channel_close(struct channel *c) {
    if (c->fd >= 0) close(c->fd);
    wipe(c, sizeof *c); c->fd = -1;
}
static int nonblock(int fd) { int flags = fcntl(fd, F_GETFL); return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
static int above_stdio(int *fd) {
    if (*fd > STDERR_FILENO) return 0;
    int moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) return -1;
    close(*fd); *fd = moved; return 0;
}
static int peer_valid(const dc_tts *t, int fd) {
    struct ucred peer; socklen_t bytes = sizeof peer;
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &bytes) && bytes == sizeof peer &&
           peer.uid == geteuid() && (!t->broker_pid || peer.pid == t->broker_pid);
}
static int channel_open(dc_tts *t, struct channel *c, unsigned type,
                         uint64_t generation, const void *payload, size_t bytes) {
    TTS_STATBUF st;
    if (TTS_LSTAT(t->socket_path, &st) || !S_ISSOCK(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0022)) { errno = EACCES; return -1; }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (above_stdio(&fd)) { int saved = errno; close(fd); errno = saved; return -1; }
    struct sockaddr_un address; memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX; strcpy(address.sun_path, t->socket_path);
    int rc = connect(fd, (struct sockaddr *)&address, sizeof address);
    if (rc && errno != EINPROGRESS && errno != EAGAIN) { int saved = errno; close(fd); errno = saved; return -1; }
    memset(c, 0, sizeof *c); c->fd = fd; c->state = rc ? 1 : 2;
    struct ntts_header h = {(uint16_t)type, 0, generation, (uint32_t)bytes, 0};
    ntts_encode(c->command, &h);
    if (bytes) memcpy(c->command + NTTS_HEADER_BYTES, payload, bytes);
    c->length = NTTS_HEADER_BYTES + bytes;
    return 0;
}
/* -1 failure, 0 still connecting/sending, 1 command sent. */
static int channel_send(dc_tts *t, struct channel *c) {
    if (c->state == 1) {
        struct pollfd p = {c->fd, POLLOUT, 0};
        int rc = poll(&p, 1, 0);
        if (rc < 0 && errno == EINTR) return 0;
        if (rc < 0) return -1;
        if (!rc) return 0;
        int error = 0; socklen_t bytes = sizeof error;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &bytes) || error) { errno = error ? error : EIO; return -1; }
        c->state = 2;
    }
    if (c->state == 2) {
        if (!peer_valid(t, c->fd)) { errno = EACCES; return -1; }
        ssize_t n = send(c->fd, c->command, c->length, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        if (n < 0 || (size_t)n != c->length) return -1;
        wipe(c->command, sizeof c->command); c->length = 0; c->state = 3;
    }
    return c->state == 3;
}
/* Reject even well-formed unexpected ancillary descriptors. */
static int channel_receive(dc_tts *t, struct channel *c, struct ntts_header *h) {
    struct iovec vector = {t->packet, sizeof t->packet};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(16 * sizeof(int))]; } ancillary;
    struct msghdr msg; memset(&msg, 0, sizeof msg);
    msg.msg_iov = &vector; msg.msg_iovlen = 1; msg.msg_control = ancillary.bytes; msg.msg_controllen = sizeof ancillary.bytes;
    ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if (n <= 0) return -1;
    int unexpected = 0;
    for (struct cmsghdr *p = CMSG_FIRSTHDR(&msg); p; p = CMSG_NXTHDR(&msg, p)) {
        unexpected = 1;
        if (p->cmsg_level == SOL_SOCKET && p->cmsg_type == SCM_RIGHTS && p->cmsg_len >= CMSG_LEN(0)) {
            size_t count = (p->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) { int fd; memcpy(&fd, (unsigned char *)CMSG_DATA(p) + i * sizeof fd, sizeof fd); close(fd); }
        }
    }
    if (unexpected || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        ntts_decode(t->packet, (size_t)n, h)) return -1;
    return 1;
}
unsigned dc_tts_retired(const dc_tts *t) {
    unsigned count = 0;
    if (t) for (unsigned i = 0; i < TTS_RETIRED_CAP; ++i) count += t->retired[i].pid > 0;
    return count;
}
void dc_tts_tick(dc_tts *t, uint64_t now) {
    if (!t) return;
    for (unsigned i = 0; i < TTS_RETIRED_CAP; ++i) {
        struct tts_retired *r = &t->retired[i]; if (!r->pid) continue;
        if (!r->killed && now >= r->kill_at) { kill(-r->pid, SIGKILL); r->killed = 1; }
        if (r->killed) {
            pid_t done = waitpid(r->pid, NULL, WNOHANG);
            if (done == r->pid || (done < 0 && errno == ECHILD)) memset(r, 0, sizeof *r);
        }
    }
}
static void decoder_close(dc_tts *t, uint64_t now) {
    if (t->input_fd >= 0) close(t->input_fd);
    if (t->output_fd >= 0) close(t->output_fd);
    t->input_fd = t->output_fd = -1;
    if (t->decoder_pid) for (unsigned i = 0; i < TTS_RETIRED_CAP; ++i) if (!t->retired[i].pid) {
        t->retired[i] = (struct tts_retired){t->decoder_pid, now + TTS_CANCEL_MS, 0};
        kill(-t->decoder_pid, SIGTERM); t->decoder_pid = 0; break;
    }
    t->active = -1; t->pcm_head = t->pcm_used = 0;
    t->eof = t->header_valid = t->decoder_done = 0;
    t->header_used = t->frame_used = t->source_bytes = t->pcm_bytes = 0;
}
static void slot_clear(struct tts_slot *s) { channel_close(&s->channel); wipe(s, sizeof *s); s->channel.fd = -1; }
static int oldest(const dc_tts *t) {
    int result = -1;
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i)
        if (t->slots[i].used && (result < 0 || t->slots[i].sequence < t->slots[result].sequence)) result = (int)i;
    return result;
}
dc_tts *dc_tts_create(const dc_tts_config *config) {
    if (!config || !config->socket_path || config->socket_path[0] != '/' || !config->decoder_path ||
        config->decoder_path[0] != '/' || strlen(config->socket_path) >= 108 ||
        strlen(config->decoder_path) >= 4096 || config->broker_pid < 0) { errno = EINVAL; return NULL; }
    TTS_STATBUF st;
    if (TTS_LSTAT(config->decoder_path, &st) || !S_ISREG(st.st_mode)) { errno = EINVAL; return NULL; }
    dc_tts *t = calloc(1, sizeof *t); if (!t) return NULL;
    strcpy(t->socket_path, config->socket_path); strcpy(t->decoder_path, config->decoder_path);
    t->broker_pid = config->broker_pid; t->warm.fd = t->input_fd = t->output_fd = t->active = -1;
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) t->slots[i].channel.fd = -1;
    return t;
}
int dc_tts_destroy(dc_tts *t) {
    if (!t) return 0;
    if (!dc_tts_quiescent(t)) { errno = EBUSY; return -1; }
    channel_close(&t->warm);
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) slot_clear(&t->slots[i]);
    if (t->input_fd >= 0) close(t->input_fd);
    if (t->output_fd >= 0) close(t->output_fd);
    wipe(t, sizeof *t); free(t); return 0;
}
void dc_tts_shutdown(dc_tts *t, uint64_t now) {
    if (!t) return;
    /* A session may own only WARM (no greeting, no reply tag). Teardown must
     * close that channel and every output, not rely on cancel(last_tag). */
    t->invalidated = 1; t->pcm_head = t->pcm_used = 0;
    channel_close(&t->warm); decoder_close(t, now);
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) slot_clear(&t->slots[i]);
    wipe(t->packet, sizeof t->packet);
}
int dc_tts_quiescent(const dc_tts *t) {
    return !t || (t->warm.fd < 0 && !dc_tts_slots(t) && t->active < 0 &&
                  !t->decoder_pid && t->input_fd < 0 && t->output_fd < 0 && !dc_tts_retired(t));
}
int dc_tts_warm(dc_tts *t, dc_tag tag, uint64_t now) {
    if (!t || !tag.session_epoch) { errno = EINVAL; return -1; }
    if (t->warm.fd >= 0) { errno = EBUSY; return -1; }
    if (channel_open(t, &t->warm, NTTS_WARM, 0, NULL, 0)) return -1;
    t->warm_tag = tag; t->warm_deadline = now + TTS_URL_MS; return 0;
}
unsigned dc_tts_slots(const dc_tts *t) {
    unsigned count = 0; if (t) for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) count += !!t->slots[i].used;
    return count;
}
int dc_tts_decoder_active(const dc_tts *t) { return t && t->active >= 0; }
int dc_tts_submit(dc_tts *t, dc_tag tag, uint64_t sequence, const char *text, uint64_t now) {
    if (!t || !tag.session_epoch || !tag.reply_generation || !tag.utterance_id || !sequence || !text) { errno = EINVAL; return -1; }
    size_t bytes = strnlen(text, NTTS_MAX_TEXT + 1);
    if (!ntts_text_valid((const unsigned char *)text, bytes)) { errno = EINVAL; return -1; }
    if (t->last_tag.session_epoch == tag.session_epoch &&
        (tag.reply_generation < t->last_tag.reply_generation ||
         (tag.reply_generation == t->last_tag.reply_generation &&
          (t->invalidated || sequence <= t->last_sequence || !dc_tag_equal(tag, t->last_tag))))) { errno = ESTALE; return -1; }
    if (dc_tts_slots(t) && !dc_tag_equal(tag, t->last_tag)) { errno = EBUSY; return -1; }
    int index = -1;
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) if (!t->slots[i].used) { index = (int)i; break; }
    if (index < 0) { errno = EAGAIN; return -1; }
    struct tts_slot *s = &t->slots[index];
    if (channel_open(t, &s->channel, NTTS_REQUEST, tag.reply_generation, text, bytes)) return -1;
    s->used = 1; s->tag = tag; s->sequence = sequence; s->deadline = now + TTS_URL_MS;
    t->last_tag = tag; t->last_sequence = sequence; t->invalidated = 0; return 0;
}
int dc_tts_next(const dc_tts *t, dc_tag *tag, uint64_t *sequence, int *ready) {
    if (!t || !tag || !sequence || !ready) { errno = EINVAL; return -1; }
    int index = oldest(t); if (index < 0) return 0;
    *tag = t->slots[index].tag; *sequence = t->slots[index].sequence; *ready = t->slots[index].ready; return 1;
}
extern char **environ;
int dc_tts_activate(dc_tts *t, dc_tag tag, uint64_t sequence, uint64_t now) {
    if (!t) { errno = EINVAL; return -1; }
    int index = oldest(t);
    if (index < 0 || !dc_tag_equal(t->slots[index].tag, tag) || t->slots[index].sequence != sequence) { errno = ESTALE; return -1; }
    if (t->active >= 0 || !t->slots[index].ready || dc_tts_retired(t) >= TTS_RETIRED_CAP) { errno = EAGAIN; return -1; }
    /* Construct the environment before fork; the child makes only async-safe
     * syscalls before exec. The old wrapper's readiness FD is not inherited. */
    char *child_env[513]; unsigned environment = 0;
    for (char **p = environ; *p; ++p) {
        if (!strncmp(*p, "MODEL_AUDIO_READY_FD=", 21)) continue;
        if (environment >= 512) { errno = E2BIG; return -1; }
        child_env[environment++] = *p;
    }
    child_env[environment] = NULL;
    int input[2], output[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input)) return -1;
    if (pipe2(output, O_CLOEXEC)) { int saved = errno; close(input[0]); close(input[1]); errno = saved; return -1; }
    if (above_stdio(&input[0]) || above_stdio(&input[1]) || above_stdio(&output[0]) || above_stdio(&output[1]) ||
        nonblock(input[1]) || nonblock(output[0])) {
        int saved = errno; close(input[0]); close(input[1]); close(output[0]); close(output[1]); errno = saved; return -1;
    }
    char *arguments[] = {t->decoder_path, NULL}; pid_t parent = getpid(), child = fork();
    if (child < 0) { int saved = errno; close(input[0]); close(input[1]); close(output[0]); close(output[1]); errno = saved; return -1; }
    if (!child) {
        struct rlimit cores = {0, 0};
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (setpgid(0, 0) || prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
            setrlimit(RLIMIT_CORE, &cores) || dup2(input[0], 0) < 0 || dup2(output[1], 1) < 0) _exit(126);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        execve(t->decoder_path, arguments, child_env); _exit(127);
    }
    close(input[0]); close(output[1]); setpgid(child, child);
    t->active = index; t->decoder_pid = child; t->input_fd = input[1]; t->output_fd = output[0];
    t->decoder_deadline = now + TTS_DECODE_MS; t->eof = t->header_valid = t->decoder_done = 0;
    t->header_used = t->frame_used = t->source_bytes = t->pcm_bytes = t->pcm_head = t->pcm_used = 0;
    return 0;
}
int dc_tts_cancel(dc_tts *t, dc_tag tag, uint64_t now) {
    if (!t || t->invalidated || !same_reply(t->last_tag, tag)) return 0;
    t->invalidated = 1; t->pcm_head = t->pcm_used = 0;
    decoder_close(t, now);
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) if (t->slots[i].used && same_reply(t->slots[i].tag, tag)) slot_clear(&t->slots[i]);
    if (t->warm.fd >= 0 && t->warm_tag.session_epoch == tag.session_epoch) channel_close(&t->warm);
    wipe(t->packet, sizeof t->packet); return 1;
}
static int slot_error(dc_tts *t, int index, uint64_t now, dc_tts_event *event, enum dc_tts_error error) {
    memset(event, 0, sizeof *event); event->type = DC_TTS_ERROR; event->tag = t->slots[index].tag;
    event->segment_sequence = t->slots[index].sequence; event->at_ms = now; event->error = error;
    if (t->active == index) decoder_close(t, now);
    slot_clear(&t->slots[index]); wipe(t->packet, sizeof t->packet); return 1;
}
static int wave_header(dc_tts *t) {
    const unsigned char *h = t->header;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) || ntts_u32(h + 16) != 16 ||
        ntts_u16(h + 20) != 1 || memcmp(h + 36, "data", 4) || ntts_u16(h + 34) != 16) return -1;
    t->channels = ntts_u16(h + 22); t->rate = ntts_u32(h + 24); t->declared_bytes = ntts_u32(h + 40);
    if ((t->channels != 1 && t->channels != 2) || t->rate < 8000 || t->rate > 48000 ||
        ntts_u16(h + 32) != t->channels * 2 || ntts_u32(h + 28) != t->rate * t->channels * 2 ||
        !t->declared_bytes || t->declared_bytes % (t->channels * 2) ||
        ntts_u32(h + 4) != t->declared_bytes + 36u ||
        (t->declared_bytes != TTS_STREAM_LENGTH && t->declared_bytes > t->rate * t->channels * 2 * 30u)) return -1;
    t->header_valid = 1; return 0;
}
static int ingest_wave(dc_tts *t, const unsigned char *bytes, size_t count) {
    while (count) {
        if (!t->header_valid) {
            size_t n = 44 - t->header_used; if (n > count) n = count;
            memcpy(t->header + t->header_used, bytes, n); t->header_used += n; bytes += n; count -= n;
            if (t->header_used == 44 && wave_header(t)) return -1;
            continue;
        }
        if (t->source_bytes >= (size_t)t->rate * t->channels * 2 * 30u ||
            (t->declared_bytes != TTS_STREAM_LENGTH && t->source_bytes >= t->declared_bytes)) return -1;
        t->frame[t->frame_used++] = *bytes++; --count; ++t->source_bytes;
        if (t->frame_used == t->channels * 2) {
            int32_t sample = (int16_t)ntts_u16(t->frame);
            if (t->channels == 2) sample = (sample + (int16_t)ntts_u16(t->frame + 2)) / 2;
            if (t->pcm_used + 2 > sizeof t->pcm) return -1;
            size_t tail = (t->pcm_head + t->pcm_used) % sizeof t->pcm;
            t->pcm[tail] = (unsigned char)(uint16_t)sample;
            t->pcm[(tail + 1) % sizeof t->pcm] = (unsigned char)((uint16_t)sample >> 8);
            t->pcm_used += 2; t->pcm_bytes += 2; t->frame_used = 0;
        }
    }
    return 0;
}
int dc_tts_peek_pcm(dc_tts *t, dc_tts_pcm_view *view) {
    if (!t || !view) { errno = EINVAL; return -1; }
    if (t->active < 0 || !t->pcm_used) return 0;
    struct tts_slot *s = &t->slots[t->active]; memset(view, 0, sizeof *view);
    view->tag = s->tag; view->segment_sequence = s->sequence; view->rate = t->rate;
    view->data = t->pcm + t->pcm_head;
    size_t count = sizeof t->pcm - t->pcm_head; if (count > t->pcm_used) count = t->pcm_used;
    if (count > DC_TTS_PCM_VIEW_MAX) count = DC_TTS_PCM_VIEW_MAX;
    view->bytes = count; return 1;
}
int dc_tts_consume_pcm(dc_tts *t, dc_tag tag, uint64_t sequence, size_t bytes) {
    if (!t || t->active < 0 || !bytes || bytes % 2) { errno = EINVAL; return -1; }
    struct tts_slot *s = &t->slots[t->active];
    if (!dc_tag_equal(tag, s->tag) || sequence != s->sequence) { errno = ESTALE; return -1; }
    size_t contiguous = sizeof t->pcm - t->pcm_head;
    if (bytes > t->pcm_used || bytes > contiguous || bytes > DC_TTS_PCM_VIEW_MAX) { errno = EINVAL; return -1; }
    t->pcm_head = (t->pcm_head + bytes) % sizeof t->pcm; t->pcm_used -= bytes; return 0;
}
int dc_tts_poll(dc_tts *t, uint64_t now, dc_tts_event *event) {
    if (!t || !event) { errno = EINVAL; return -1; }
    memset(event, 0, sizeof *event); dc_tts_tick(t, now);
    if (t->warm.fd >= 0) {
        int sent = channel_send(t, &t->warm), rc = 0; struct ntts_header h;
        if (sent > 0) rc = channel_receive(t, &t->warm, &h);
        if (sent < 0 || rc < 0 || now >= t->warm_deadline || rc > 0) {
            event->tag = t->warm_tag; event->at_ms = now;
            event->type = rc > 0 && h.type == NTTS_READY && h.request_id && !h.payload_len && !h.code ? DC_TTS_WARM_READY : DC_TTS_ERROR;
            event->error = event->type == DC_TTS_ERROR ? DC_TTS_ERR_BROKER : 0;
            channel_close(&t->warm); wipe(t->packet, sizeof t->packet); return 1;
        }
    }
    for (unsigned i = 0; i < DC_TTS_SLOTS; ++i) {
        struct tts_slot *s = &t->slots[i]; if (!s->used || s->ready) continue;
        if (now >= s->deadline) return slot_error(t, (int)i, now, event, DC_TTS_ERR_TIMEOUT);
        int sent = channel_send(t, &s->channel); struct ntts_header h;
        if (sent < 0) return slot_error(t, (int)i, now, event, DC_TTS_ERR_IO);
        if (!sent) continue;
        int rc = channel_receive(t, &s->channel, &h);
        if (rc < 0) return slot_error(t, (int)i, now, event, DC_TTS_ERR_PROTOCOL);
        if (!rc) continue;
        if (h.type == NTTS_STATUS && !h.payload_len) return slot_error(t, (int)i, now, event, DC_TTS_ERR_BROKER);
        if (h.type != NTTS_URL || h.code || !h.request_id || h.generation != s->tag.reply_generation ||
            !ntts_url_valid(t->packet + NTTS_HEADER_BYTES, h.payload_len)) return slot_error(t, (int)i, now, event, DC_TTS_ERR_PROTOCOL);
        memcpy(s->url, t->packet + NTTS_HEADER_BYTES, h.payload_len); s->url_length = h.payload_len;
        s->url_at = 0; s->ready = 1; channel_close(&s->channel); wipe(t->packet, sizeof t->packet);
        event->type = DC_TTS_URL_READY; event->tag = s->tag; event->segment_sequence = s->sequence; event->at_ms = now; return 1;
    }
    if (t->active >= 0) {
        int index = t->active; struct tts_slot *s = &t->slots[index];
        if (now >= t->decoder_deadline) return slot_error(t, index, now, event, DC_TTS_ERR_TIMEOUT);
        if (t->input_fd >= 0) {
            ssize_t n = send(t->input_fd, s->url + s->url_at, s->url_length - s->url_at, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) s->url_at += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return slot_error(t, index, now, event, DC_TTS_ERR_IO);
            if (s->url_at == s->url_length) { close(t->input_fd); t->input_fd = -1; wipe(s->url, sizeof s->url); }
        }
        if (!t->eof && sizeof t->pcm - t->pcm_used >= 2) {
            unsigned char incoming[4096]; size_t capacity = (sizeof t->pcm - t->pcm_used) & ~(size_t)1;
            if (capacity > sizeof incoming) capacity = sizeof incoming;
            ssize_t n = read(t->output_fd, incoming, capacity);
            if (n > 0 && ingest_wave(t, incoming, (size_t)n)) return slot_error(t, index, now, event, DC_TTS_ERR_PROTOCOL);
            if (!n) t->eof = 1;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return slot_error(t, index, now, event, DC_TTS_ERR_IO);
        }
        if (t->eof && !t->decoder_done) {
            if (!t->header_valid || t->frame_used || !t->pcm_bytes ||
                (t->declared_bytes != TTS_STREAM_LENGTH && t->source_bytes != t->declared_bytes))
                return slot_error(t, index, now, event, DC_TTS_ERR_PROTOCOL);
            siginfo_t info; memset(&info, 0, sizeof info);
            int rc = waitid(P_PID, (id_t)t->decoder_pid, &info, WEXITED | WNOHANG | WNOWAIT);
            if (rc && errno == EINTR) return 0;
            if (rc || (info.si_pid && (info.si_code != CLD_EXITED || info.si_status))) return slot_error(t, index, now, event, DC_TTS_ERR_DECODER);
            if (info.si_pid) {
                kill(-t->decoder_pid, SIGKILL);
                if (waitpid(t->decoder_pid, NULL, WNOHANG) != t->decoder_pid) return slot_error(t, index, now, event, DC_TTS_ERR_IO);
                t->decoder_pid = 0; close(t->output_fd); t->output_fd = -1; t->decoder_done = 1;
            }
        }
        if (t->decoder_done && !t->pcm_used) {
            event->type = DC_TTS_SEGMENT_DONE; event->tag = s->tag;
            event->segment_sequence = s->sequence; event->rate = t->rate; event->at_ms = now;
            slot_clear(s); decoder_close(t, now); return 1;
        }
    }
    return 0;
}
size_t dc_tts_storage_bytes(void) { return sizeof(dc_tts); }
