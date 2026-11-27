/* Passive copy of the stock post-AEC/beamforming/DNN mono stream, before VAD.
 * The original function, arguments, return value and errno are preserved.
 * Every consumer has its own nonblocking datagram socket. No VOIP events,
 * FIFO readers, cloud-socket proxy, allocation or blocking writes on this path.
 * ABI verified against libxaudio_engine.so xaudio_dnn_thread -> wrapper_dnn.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __arm__
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
#endif
#ifndef TAPDIR
#define TAPDIR "/tmp/lx06-mic"
#endif
#ifdef LX06_ACCOUNT_MIC_LXA2
#include "../../runtime/model-adapter/experiments/account-mic-publisher-lifecycle.h"
#endif
#define MAX_CLIENTS 8
#define TOTAL_CLIENTS (MAX_CLIENTS * 2)
#define MAX_PCM 4096
typedef int (*dnn_fn)(void *, const void *, int, void *, int *);
static dnn_fn original;
static unsigned sequence, scan_count;
static unsigned char generation[16];
static uint64_t kws_sequence;
static pid_t generation_pid;
static int generation_failed;
/* Never block the audio thread or substitute timestamps/PIDs for entropy.
 * Entropy failures are retried; sequence exhaustion remains fail-closed. */
static int kws_identity(void) {
    pid_t pid = getpid();
    if (generation_pid != pid) {
        generation_pid = pid; generation_failed = 0; kws_sequence = 0;
        memset(generation, 0, sizeof(generation));
    }
    if (generation_failed) return 0;
    if (!memcmp(generation, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) {
#ifdef SYS_getrandom
        unsigned char candidate[16];
        ssize_t n = syscall(SYS_getrandom, candidate, sizeof(candidate), 1 /* GRND_NONBLOCK */);
        if (n != sizeof(candidate) || !memcmp(candidate, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16)) return 0;
        memcpy(generation, candidate, sizeof(generation));
#else
        return 0;
#endif
    }
    return 1;
}
static void put_le(unsigned char *p, uint64_t v, unsigned n) {
    while (n--) { *p++ = (unsigned char)v; v >>= 8; }
}
static struct subscriber {
    struct sockaddr_un address;
    int sender, active, seen, kws;
} clients[TOTAL_CLIENTS];
static int has_timestamped_stock;
extern void lx06_native_poll(void);
extern void lx06_native_audio(const void *,unsigned);

static void rescan(void) {
    DIR *dir = opendir(TAPDIR);
    struct dirent *ent;
    for (int i=0;i<TOTAL_CLIENTS;i++) clients[i].seen=0;
    if (!dir) return;
    while ((ent = readdir(dir))) {
        size_t len = strlen(ent->d_name);
        int kws = len >= 4 && !strcmp(ent->d_name + len - 3, ".k3");
        if(!strcmp(ent->d_name,"kws.kt1")) kws=2;
        if (!kws && (len < 6 || strcmp(ent->d_name + len - 5, ".sock"))) continue;
        if (len + sizeof(TAPDIR) + 1 >= sizeof(clients[0].address.sun_path)) continue;
        char path[sizeof(clients[0].address.sun_path)];
        snprintf(path,sizeof(path),"%s/%s",TAPDIR,ent->d_name);
        int slot=-1;
        for(int i=0;i<TOTAL_CLIENTS;i++)
            if(clients[i].active && !strcmp(clients[i].address.sun_path,path)) { slot=i; break; }
        if(slot>=0) { clients[slot].seen=1; continue; }
        int first = kws ? MAX_CLIENTS : 0;
        for(int i=first;i<first+MAX_CLIENTS;i++) if(!clients[i].active) { slot=i; break; }
        if(slot<0) continue;
        int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd<0) continue;
        clients[slot].sender=fd; clients[slot].active=clients[slot].seen=1;
        clients[slot].kws=kws;
        struct sockaddr_un *address = &clients[slot].address;
        memset(address, 0, sizeof(*address));
        address->sun_family = AF_UNIX;
        snprintf(address->sun_path, sizeof(address->sun_path), "%s/%s", TAPDIR, ent->d_name);
    }
    closedir(dir);
    for(int i=0;i<TOTAL_CLIENTS;i++) if(clients[i].active && !clients[i].seen) {
        close(clients[i].sender); clients[i].active=0;
    }
    has_timestamped_stock=0;
    for(int i=MAX_CLIENTS;i<TOTAL_CLIENTS;i++)
        if(clients[i].active && clients[i].kws==2) has_timestamped_stock=1;
}

int xaudio_wrapper_dnn(void *context, const void *input, int bytes, void *output, int *output_bytes) {
    if (!original) original = (dnn_fn)dlsym(RTLD_NEXT, "xaudio_wrapper_dnn");
    if (!original) { errno = ENOSYS; return -1; }
    int result = original(context, input, bytes, output, output_bytes);
    int saved_errno = errno;
    /* LX06 AFE_DnnProcessPostAgc returns two PLANAR 160-sample channels:
     * processed speech at +0, reference at +320 (verified disassembly).
     * Passing both as mono doubles time and inserts reference between words.
     */
    if (result == 0 && output && output_bytes && *output_bytes == 640) {
        /* Mark producer completion before native consumers, directory scans,
         * socket buffering or downstream MFCC work can delay this block. */
        uint64_t stock_ms=0;
        struct { long sec,nsec; } now;
        if(has_timestamped_stock && !syscall(SYS_clock_gettime,1,&now))
            stock_ms=(uint64_t)now.sec*1000+(unsigned long)now.nsec/1000000;
#ifdef LX06_ACCOUNT_MIC_LXA2
        lxaf_publish(output, 320);
#endif
        struct { uint32_t magic, seq, rate, bytes; unsigned char pcm[MAX_PCM]; } packet;
        if (!(scan_count++ % 16)) rescan();
        packet.magic = 0x3141584c; /* LXA1, little endian */
        packet.seq = sequence++;
        packet.rate = 16000;
        packet.bytes = 320;
        memcpy(packet.pcm, output, packet.bytes);
        lx06_native_audio(output,packet.bytes);
        unsigned char k3[36 + 320 + 8];
        int has_kws = 0;
        for (int i = MAX_CLIENTS; i < TOTAL_CLIENTS; ++i)
            if (clients[i].active) { has_kws = 1; break; }
        int k3_ok = has_kws && kws_identity();
        if (k3_ok) {
            memcpy(k3, "KWS3", 4); memcpy(k3 + 4, generation, 16);
            put_le(k3 + 20, kws_sequence, 8); put_le(k3 + 28, 16000, 4);
            put_le(k3 + 32, 320, 4); memcpy(k3 + 36, output, 320);
            put_le(k3+356,stock_ms,8);
            if (kws_sequence == UINT64_MAX) generation_failed = 1;
            else ++kws_sequence;
        }
        /* Per-subscriber send buffers are essential: one stalled receiver must
         * not exhaust the sender's shared socket memory for healthy readers. */
        for (int i = 0; i < TOTAL_CLIENTS; ++i) {
            if(!clients[i].active || (clients[i].kws && !k3_ok)) continue;
            const void *data=&packet;
            size_t size=16+packet.bytes;
            if(clients[i].kws) {
                int timed=clients[i].kws==2;
                if(timed && !stock_ms) continue;
                memcpy(k3,timed ? "KWT1" : "KWS3",4);
                data=k3;size=timed ? sizeof(k3) : 356;
            }
            (void)sendto(clients[i].sender,data,size,MSG_DONTWAIT|MSG_NOSIGNAL,
                         (struct sockaddr *)&clients[i].address,sizeof(clients[i].address));
        }
    }
    lx06_native_poll();
    errno = saved_errno;
    return result;
}

#if defined(LX06_RAW_WAKE_CAPTURE_HISTORY_FRAMES) && (defined(__arm__) || defined(LX06_RAW_WAKE_CAPTURE_TEST))
#include "raw-wake-capture.inc"
#endif
