/* Only the auxiliary physical stream goes through this private NS instance.
 * OEM ALSA reads enqueue whole packets without waiting for the worker. */
#include <pthread.h>
#include "wake-oem-ns-stream.h"
static int ns_input = -1;
static pid_t ns_owner;
static struct wake_ns_stream ns_state;
/* This root-only, opt-in snapshot contains counters only. No path is
 * created by the worker. In particular the ALSA capture thread never logs. */
static unsigned ns_drop,ns_gap,ns_source,ns_stale,ns_owner_busy,ns_epoch,ns_error,ns_recovered;
static void ns_diagnostics(void) {
    static uint64_t last;
    static unsigned reported;
    unsigned drop=__atomic_load_n(&ns_drop,__ATOMIC_RELAXED);
    unsigned total=drop+ns_gap+ns_source+ns_stale+ns_owner_busy+ns_epoch+ns_error+ns_recovered;
    if(reported==total) return;
    uint64_t now=lx06_idle_epoch_now_ms();
    if(now-last<250) return;
    last=now;reported=total;
    int fd=open("/tmp/lx06-ns-stats",O_WRONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) return;
    LX06_SESSION_STATBUF st;
    if(!LX06_EPOCH_FSTAT(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
       (st.st_mode&0777)==0600 && st.st_nlink==1) {
        char text[192];
        int n=snprintf(text,sizeof(text),"input_drop=%u gap=%u source=%u stale=%u busy=%u epoch=%u error=%u recovered=%u\n",
                drop,ns_gap,ns_source,ns_stale,ns_owner_busy,ns_epoch,ns_error,ns_recovered);
        if(n>0 && n<(int)sizeof(text)) (void)pwrite(fd,text,(size_t)n,0);
    }
    close(fd);
}

static int ns_publish(const uint8_t *packet, size_t size, void *unused) {
    (void)unused;
    uint8_t idle[16];
    uint64_t available = wake_ns_le64(packet + LX06_WAKE_FRAME_AVAILABLE);
    uint64_t now = lx06_idle_epoch_now_ms();
    if (size != LX06_WAKE_FRAME_BYTES || !now || available > now || now - available > 200) {
        ns_stale++;return -1;
    }
    if(lx06_idle_epoch_snapshot(idle)) {
        /* A status probe may briefly hold the lock without starting a
         * session. Retry the full ownership check on this worker only. */
        if(errno!=EWOULDBLOCK || raw_idle_snapshot(idle)) {ns_owner_busy++;return -1;}
        now=lx06_idle_epoch_now_ms();
        if(!now || available>now || now-available>200) {ns_stale++;return -1;}
        if(!memcmp(idle,packet+LX06_WAKE_FRAME_IDLE,sizeof(idle))) ns_recovered++;
    }
    if(memcmp(idle,packet+LX06_WAKE_FRAME_IDLE,sizeof(idle))) {ns_epoch++;return -1;}
    raw_publish(packet, wake_ns_le64(packet + LX06_WAKE_FRAME_FIRST), available);
    return 0;
}

static void *ns_work(void *arg) {
    int fd = (int)(intptr_t)arg;
    uint8_t frame[LX06_WAKE_FRAME_BYTES];
    for (;;) {
        ssize_t size = recv(fd, frame, sizeof(frame), MSG_TRUNC);
        if (size < 0 && errno == EINTR) continue;
        if (size != sizeof(frame)) break;
        if(ns_state.ns.handle) {
            if(memcmp(ns_state.input_id,frame+LX06_WAKE_FRAME_SOURCE,32)) ns_source++;
            else if(ns_state.next_input!=wake_ns_le64(frame+LX06_WAKE_FRAME_FIRST)) ns_gap++;
        }
        if(wake_ns_frame(&ns_state, frame, ns_publish, NULL)<0) ns_error++;
        ns_diagnostics();
    }
    wake_ns_end(&ns_state);
    close(fd);
    return NULL;
}

static int ns_worker_start(void) {
    static const char producer[] = "/usr/bin/mipns-xiaomi";
    char exe[sizeof(producer)];
    if (readlink("/proc/self/exe", exe, sizeof(exe)) != (ssize_t)sizeof(producer)-1 ||
        memcmp(exe, producer, sizeof(producer)-1)) return 0;
    pthread_t thread;
    pthread_attr_t attr;
    /* Fit all frames eligible under the existing 200ms freshness bound.
     * UNIX datagrams also charge per-packet kernel memory beyond 692 B. */
    int fd[2], capacity = 16384;
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fd)) return 0;
    int ok = !setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) &&
             !pthread_attr_init(&attr);
    if (ok) {
        ok = !pthread_attr_setstacksize(&attr, 96 * 1024) &&
             !pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) &&
             !pthread_create(&thread, &attr, ns_work, (void *)(intptr_t)fd[1]);
        pthread_attr_destroy(&attr);
    }
    if (!ok) { close(fd[0]); close(fd[1]); return 0; }
    ns_owner = getpid();
    ns_input = fd[0];
    return 1;
}
