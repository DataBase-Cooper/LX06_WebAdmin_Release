/* Independent subscriber to processed-mic-tap. Never opens/closes a session.
 * Readiness requires a real valid PCM packet; enrollment may then open a gate.
 * Output is unmodified mono 16k S16LE. Discontinuities are reported, not hidden.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include "glibc_ni_trap.h"
#include "wake-capture-frame.h"

#ifndef KWS_PRODUCER_EXE
#define KWS_PRODUCER_EXE "/usr/bin/mipns-xiaomi"
#endif

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }
static long long now_ms(void) {
    struct { long sec, nsec; } stamp;
    if (syscall(SYS_clock_gettime, 1, &stamp) != 0) return 0;
    return (long long)stamp.sec * 1000 + stamp.nsec / 1000000;
}
static int write_all(int fd, const void *ptr, size_t bytes) {
    const unsigned char *p = ptr;
    while (bytes && !stopped) {
        ssize_t n = write(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n;
    }
    return stopped ? -1 : 0;
}
static uint64_t get_le(const unsigned char *p, unsigned n) {
    uint64_t v = 0;
    while (n) { v = (v << 8) | p[--n]; }
    return v;
}

/* The generation is public continuity data. Bind every framed datagram to
 * the stock process using Linux sender credentials. The frame contract and
 * its producer, rather than these credentials, determine capture provenance. */
static int kws_receive(int fd, unsigned char *raw, size_t bytes, uid_t owner, pid_t *producer) {
    struct iovec iov = { .iov_base = raw, .iov_len = bytes };
    union {
        struct cmsghdr aligned;
        unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))];
    } control;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    if (recvmsg(fd, &msg, MSG_TRUNC | MSG_CMSG_CLOEXEC) != (ssize_t)bytes ||
        (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) return -1;
    struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
    if (!header || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_CREDENTIALS ||
        header->cmsg_len != CMSG_LEN(sizeof(struct ucred)) ||
        CMSG_NXTHDR(&msg, header)) return -1;
    struct ucred credential;
    memcpy(&credential, CMSG_DATA(header), sizeof(credential));
    if (credential.pid <= 1 || credential.uid != owner) return -1;
    if (*producer) return credential.pid == *producer ? 0 : -1;
    char path[48], executable[sizeof(KWS_PRODUCER_EXE)];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)credential.pid);
    ssize_t length = readlink(path, executable, sizeof(executable));
    if (length != (ssize_t)sizeof(executable) - 1 ||
        memcmp(executable, KWS_PRODUCER_EXE, sizeof(executable) - 1)) return -1;
    *producer = credential.pid;
    return 0;
}

#include "paired-reader.inc"
int main(int argc, char **argv) {
    if(argc==4 && !strcmp(argv[1],"--name") && !strcmp(argv[2],"kws") &&
       !strcmp(argv[3],"--kws-paired")) return read_paired();
    const char *name = NULL, *ready = NULL, *gate = NULL, *output = NULL;
    int duration = 0, preroll_ms = 0, kws = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--ready-file") && i + 1 < argc) ready = argv[++i];
        else if (!strcmp(argv[i], "--gate-file") && i + 1 < argc) gate = argv[++i];
        else if (!strcmp(argv[i], "--dur") && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preroll-ms") && i + 1 < argc) preroll_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "--kws-framed") && kws != 2) kws = 1;
        else if (!strcmp(argv[i], "--kws-epoch") && kws != 1) kws = 2;
        else return 2;
    }
    if (!name || !*name || strlen(name) > 32 || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != strlen(name)) return 2;
    int epoch_enrollment = kws == 2 && !strcmp(name, "enrollment");
    if (kws && (output || (!epoch_enrollment && (strcmp(name,"kws") || ready || gate || duration || preroll_ms)))) return 2;
    if (epoch_enrollment && (duration < 0 || duration > 30 || preroll_ms % 20)) return 2;
    if (preroll_ms < 0 || preroll_ms > 1000) return 2;
    unsigned char preroll[50 * LX06_WAKE_FRAME_BYTES];
    size_t pre_cap = epoch_enrollment ? (size_t)(preroll_ms / 20) * LX06_WAKE_FRAME_BYTES : (size_t)preroll_ms * 32;
    size_t pre_used = 0;
    mkdir("/tmp/lx06-mic", 0755);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
    const char *extension = kws == 2 ? "ke2" : kws == 1 ? "k3" : "sock";
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/lx06-mic/%s.%s", name, extension);
    /* Keep the lock inode after exit: unlinking it permits simultaneous owners.
     * SIGKILL releases flock; the next owner can recover a stale socket safely.
     */
    char lockpath[128];
    snprintf(lockpath, sizeof(lockpath), "/tmp/lx06-mic/%s%s.lock", name, kws == 2 ? ".ke2" : "");
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) { perror("mic owner"); return 1; }
    int probe = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return 1;
    if (connect(probe, (struct sockaddr *)&address, sizeof(address)) == 0) {
        fprintf(stderr, "processed-mic: subscriber already active\n"); close(probe); return 1;
    }
    int stale = errno == ECONNREFUSED || errno == ENOENT;
    close(probe);
    if (!stale) { fprintf(stderr, "processed-mic: cannot verify old socket\n"); return 1; }
    unlink(address.sun_path);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    int passcred = 1;
    if (kws && setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) != 0) {
        perror("mic producer credentials"); close(fd); return 1;
    }
    /* An active subscriber owns its name. Never unlink another live reader. */
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) { perror("mic bind"); close(fd); return 1; }
    int buffer_bytes = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes));
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
    int ofd = output ? open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644) : STDOUT_FILENO;
    int rc = 0, announced = 0, opened = gate == NULL;
    uint32_t previous = 0;
    uint64_t previous_kws = 0;
    pid_t kws_producer = 0;
    uid_t kws_owner = kws ? geteuid() : 0;
    unsigned char previous_generation[16] = {0};
    unsigned packets = 0, lost = 0;
    long long start = now_ms(), last = start, max_gap = 0;
    while (ofd >= 0 && !stopped && (!duration || now_ms() - start < duration * 1000LL)) {
        struct pollfd p = { fd, POLLIN, 0 };
        int status = poll(&p, 1, 100);
        if (status < 0 && errno == EINTR) continue;
        if (status < 0) { rc = 1; break; }
        if (!status) {
            if (now_ms() - last > 3000) { fprintf(stderr, "processed-mic: no real frames for 3s\n"); rc = 3; break; }
            continue;
        }
        struct { uint32_t magic, seq, rate, bytes; unsigned char pcm[4096]; } packet;
        if (kws == 2) {
            unsigned char raw[LX06_WAKE_FRAME_BYTES];
            if (kws_receive(fd, raw, sizeof(raw), kws_owner, &kws_producer) || memcmp(raw, LX06_WAKE_FRAME_MAGIC, 4)) {
                fprintf(stderr, "processed-mic: invalid epoch frame\n"); rc = 3; break;
            }
            /* Preserve the authenticated header across the private pipe.
             * detect-epoch owns continuity, sample extent and epoch checks. */
            if (pre_used) {
                const unsigned char *prev = preroll + pre_used - LX06_WAKE_FRAME_BYTES;
                if (memcmp(prev + LX06_WAKE_FRAME_SOURCE, raw + LX06_WAKE_FRAME_SOURCE, 32) ||
                    get_le(raw + LX06_WAKE_FRAME_FIRST, 8) - get_le(prev + LX06_WAKE_FRAME_FIRST, 8) != LX06_WAKE_FRAME_SAMPLES)
                    pre_used = 0;
            }
            memcpy(packet.pcm, raw, sizeof(raw));
            packet.bytes = sizeof(raw);
            packet.seq = 0;
        } else if (kws == 1) {
            unsigned char raw[356];
            static const unsigned char zero[16] = {0};
            if (kws_receive(fd, raw, sizeof(raw), kws_owner, &kws_producer) ||
                memcmp(raw, "KWS3", 4) || !memcmp(raw + 4, zero, 16) ||
                get_le(raw + 28, 4) != 16000 || get_le(raw + 32, 4) != 320 ||
                (announced && (memcmp(raw + 4, previous_generation, 16) || previous_kws == UINT64_MAX || get_le(raw + 20, 8) != previous_kws + 1))) {
                fprintf(stderr, "processed-mic: KWS3 discontinuity or malformed frame\n"); rc = 3; break;
            }
            memcpy(previous_generation, raw + 4, 16); previous_kws = get_le(raw + 20, 8);
            packet.seq = 0; packet.bytes = 320; memcpy(packet.pcm, raw + 36, 320);
        } else {
            ssize_t n = recv(fd, &packet, sizeof(packet), 0);
            if (n < 16 || packet.magic != 0x3141584c || packet.rate != 16000 ||
                !packet.bytes || packet.bytes > sizeof(packet.pcm) || (packet.bytes & 1) || n != (ssize_t)(16 + packet.bytes)) continue;
        }
        long long tick = now_ms(), gap = tick - last;
        if (packets && gap > max_gap) max_gap = gap;
        if (!kws && packets && packet.seq != previous + 1) lost += packet.seq - previous - 1;
        previous = packet.seq; last = tick; ++packets;
        if (!announced) {
            if (ready) { int marker = open(ready, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (marker < 0) { rc = 1; break; } close(marker); }
            fprintf(stderr, "processed-mic: READY bytes=%u rate=16000\n", packet.bytes);
            announced = 1;
        }
        if (!opened && access(gate, F_OK) == 0) {
            opened = 1;
            if (pre_used && write_all(ofd, preroll, pre_used) != 0) { rc = 1; break; }
            pre_used = 0;
        }
        if (!opened && pre_cap) {
            size_t keep = packet.bytes < pre_cap ? packet.bytes : pre_cap;
            if (pre_used + keep > pre_cap) {
                size_t drop = pre_used + keep - pre_cap;
                memmove(preroll, preroll + drop, pre_used - drop); pre_used -= drop;
            }
            memcpy(preroll + pre_used, packet.pcm + packet.bytes - keep, keep);
            pre_used += keep;
        }
        if (opened && write_all(ofd, packet.pcm, packet.bytes) != 0) { rc = stopped || errno == EPIPE ? 0 : 1; break; }
    }
    if (ofd < 0) rc = 1;
    if (output && ofd >= 0) close(ofd);
    close(fd); unlink(address.sun_path);
    close(lockfd);
    if (ready) unlink(ready);
    fprintf(stderr, "processed-mic: packets=%u lost=%u max_gap_ms=%lld elapsed_ms=%lld\n", packets, lost, max_gap, now_ms()-start);
    return rc;
}
