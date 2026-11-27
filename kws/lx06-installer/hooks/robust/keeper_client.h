/* LX06 keeper 本地协议：只用于可信 root 管理端，不暴露 TCP 端口。
 * 固定大小 SOCK_SEQPACKET 报文，禁止拼接 shell 命令。所有调用均有截止时间；
 * keeper 不在/状态不可读时拒绝启动，不退回 root 直接执行。
 */
#ifndef LX06_KEEPER_CLIENT_H
#define LX06_KEEPER_CLIENT_H
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>

#ifndef LK_SOCKET
#define LK_SOCKET "/var/run/lx06-keeper.sock"
#endif
#define LK_MAGIC 0x4c4b3031U
#define LK_MAX_JOBS 8
/* Persistent application identities are separate from concurrent jobs.
 * Keep the 8-job wire reply ABI stable for installed clients. */
#define LK_MAX_SLOTS 32
#define LK_UID_BASE 61000
#define LK_NAME_LEN 65
#define LK_ARGC 16
#define LK_ARG_LEN 512
#define LK_AUTO 1U
#define LK_WIDE_VM 2U /* 显式 profile；不按文件名/语言自动放宽虚拟地址限制。 */
#define LK_LOG_OFF 4U
#define LK_ROOT 8U /* 部署方显式声明（脚本标记 # LX06-RUN: root）：job 以 root 执行，
                   * 不做 UID 隔离；其余保护（NO_NEW_PRIVS/rlimits/oom/优先级）保留，
                   * 停止/监管改按 pid 树（uid=0 全系统共享，UID 巡检失效）。 */

enum lk_action { LK_START = 1, LK_STOP, LK_STATUS, LK_RECOVER, LK_FORGET };
struct lk_request {
    uint32_t magic, action, flags, argc;
    uint32_t timeout_sec, reserved;
    char name[LK_NAME_LEN];
    char argv[LK_ARGC][LK_ARG_LEN];
};
struct lk_job_info {
    int64_t pid;
    uint64_t start_ticks, rss_kb;
    uint32_t uid, members;
    char name[LK_NAME_LEN];
};
struct lk_reply {
    uint32_t magic;
    int32_t error; /* errno；0=成功。拒绝启动不是成功的空 PID。 */
    int64_t pid;
    uint64_t start_ticks, rss_kb, budget_kb;
    uint32_t safe_mode, jobs;
    char reason[160];
    struct lk_job_info job[LK_MAX_JOBS];
};

static inline int64_t lk_now_ms(void) {
#if defined(__arm__)
    /* LX06 内核 4.9.61（Amlogic）没有 ARM clock_gettime64（号 403，5.1 起新增）：
       musl/glibc 的 time64 构建会先发 403，内核 do_ni_syscall 打 oops
       （主循环 ~50 次/秒，刷屏 dmesg 与 /var/log/messages）再回退旧号。
       这里直接发旧 __NR_clock_gettime（263，32 位 time_t ABI：int sec; long nsec）。
       真机探针已确认 4.9.61 上 263=clock_gettime、264=clock_getres（2026-10-18）。 */
    struct { int32_t tv_sec; int32_t tv_nsec; } ts;
    if (syscall(263, CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
static inline int lk_wait(int fd, short events, int64_t until) {
    struct pollfd p = {fd, events, 0};
    for (;;) {
        int64_t left = until - lk_now_ms();
        int n;
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        n = poll(&p, 1, left > 1000 ? 1000 : (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) continue;
        if (p.revents & events) return 0;
        errno = ECONNRESET;
        return -1;
    }
}
/* timeout_ms 是整次 RPC 的总预算，不随 EINTR、短包或慢客户端重新计时。 */
static inline int lk_call(const struct lk_request *req, struct lk_reply *reply,
                          int timeout_ms) {
    struct sockaddr_un sa;
    struct ucred peer;
    socklen_t len = sizeof(peer);
    int fd, err = 0;
    int64_t now = lk_now_ms(), until;
    ssize_t n;
    if (now < 0 || timeout_ms < 1) { errno = EINVAL; return -1; }
    until = now + timeout_ms;
    memset(reply, 0, sizeof(*reply));
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(LK_SOCKET) >= sizeof(sa.sun_path)) { err = ENAMETOOLONG; goto out; }
    strcpy(sa.sun_path, LK_SOCKET);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) { err = errno; goto out; }
        if (lk_wait(fd, POLLOUT, until) != 0) { err = errno; goto out; }
        len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) { err = errno; goto out; }
        if (err) goto out;
    }
    len = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0) { err = errno; goto out; }
    if (peer.uid != 0) { err = EPERM; goto out; }
    if (lk_wait(fd, POLLOUT, until) != 0) { err = errno; goto out; }
    n = send(fd, req, sizeof(*req), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(*req)) { err = n < 0 ? errno : EPROTO; goto out; }
    if (lk_wait(fd, POLLIN, until) != 0) { err = errno; goto out; }
    n = recv(fd, reply, sizeof(*reply), MSG_TRUNC);
    if (n != (ssize_t)sizeof(*reply) || reply->magic != LK_MAGIC || reply->jobs > LK_MAX_JOBS) {
        err = n < 0 ? errno : EPROTO;
        goto out;
    }
    if (reply->error) err = reply->error > 0 ? reply->error : EPROTO;
out:
    close(fd);
    if (err) { errno = err; return -1; }
    return 0;
}
#endif
