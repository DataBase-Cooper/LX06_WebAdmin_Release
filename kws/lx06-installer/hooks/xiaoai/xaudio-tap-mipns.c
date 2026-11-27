/* xaudio-tap-mipns.c · V44 mipns 数据面 tap · armhf · 单 socket 双向按源地址分流
 *
 * V44 音频数据面实测（2026-10-26 夜）：
 *   - mico_aivs_lab 客户端是对话必需网关，但其 /tmp/mico_aivs_lab/usock/speech.usock
 *     不承载音频（tap 占持它时对话照常、tap 零字节）；
 *   - 真实数据面 = 引擎绑定的 /tmp/mipns/usock/speech.usock：客户端向其发请求，
 *     引擎回包含麦克风 PCM 帧（pcm16 16k，V44 pns 已去 -r opus32，官方云实证兼容）。
 *
 * 本 tap 与 V28 xaudio-tap 的差异（方向模型翻转）：
 *   - 单一 ls = 原生 mipns 路径，两个方向都走它：
 *       · 客户端请求（源地址=无名/autobind）→ 记录 client_addr → 转发真引擎(mipns2)
 *       · 引擎回包（源地址=mipns2 绑定路径）→ fanout 复制给 taps/* 消费者
 *         → 回送 client_addr（最后一次请求方）
 *   - 无 labclient/agent 双 socket；持续送流、持续识别、不门控（用户定规）。
 *
 * 参数：--listen <path>(默认 /tmp/mipns/usock/speech.usock)
 *       --engine <path>(真引擎路径, 默认 /tmp/mipns2/usock/speech.usock)
 *       --tapdir <dir>(默认 /tmp/xiaoai-agent/taps)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXD 65536

static const char *listen_path = "/tmp/mipns/usock/speech.usock";
static const char *engine_path = "/tmp/mipns2/usock/speech.usock";
static const char *tapdir      = "/tmp/xiaoai-agent/taps";

static struct sockaddr_un client_addr; /* 最后一次客户端请求方（可能无名/autobind） */
static socklen_t client_len = 0;
static int have_client = 0;

/* ---- tap 消费端缓存（connect 的 dgram socket；离线时 send 报错即跳过） ---- */
#define MAX_TAPS 8
#define RESCAN_EVERY 512
static int taps[MAX_TAPS];
static int n_taps = 0;
static int taps_dirty = 1;
static unsigned tap_pkts = 0;
static unsigned g_fanout_calls = 0, g_fanout_ok = 0, g_fanout_err = 0;
static int g_last_errno = 0;
static unsigned g_cli_pkts = 0, g_eng_pkts = 0;

static int cmp_sun(const struct sockaddr_un *a, const struct sockaddr_un *b, socklen_t la, socklen_t lb)
{
    if (la != lb) return 1;
    return memcmp(a, b, la) != 0;
}

/* 为了 sendto 路径，保存每个 tap 的地址 */
static struct sockaddr_un tap_addr[MAX_TAPS];
static socklen_t tap_alen[MAX_TAPS];

static void taps_rescan(void)
{
    int i;
    DIR *d;
    struct dirent *e;
    char path[224];
    for (i = 0; i < n_taps; i++) if (taps[i] >= 0) close(taps[i]);
    n_taps = 0;
    taps_dirty = 0;
    d = opendir(tapdir);
    if (!d) return;
    while ((e = readdir(d)) != NULL && n_taps < MAX_TAPS) {
        size_t L = strlen(e->d_name);
        struct sockaddr_un *u;
        if (L < 7 || strcmp(e->d_name + L - 6, ".usock") != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", tapdir, e->d_name);
        {
            int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (fd < 0) continue;
            u = &tap_addr[n_taps];
            memset(u, 0, sizeof(*u));
            u->sun_family = AF_UNIX;
            strncpy(u->sun_path, path, sizeof(u->sun_path) - 1);
            taps[n_taps] = fd;
            tap_alen[n_taps] = (socklen_t)sizeof(*u);
            n_taps++;
        }
    }
    closedir(d);
}

static void taps_fanout(const uint8_t *buf, size_t n)
{
    int i;
    if (taps_dirty) taps_rescan();
    else if (++tap_pkts % RESCAN_EVERY == 0) { taps_dirty = 1; taps_rescan(); }
    g_fanout_calls++;
    for (i = 0; i < n_taps; i++) {
        ssize_t r = sendto(taps[i], buf, n, MSG_NOSIGNAL, &tap_addr[i], tap_alen[i]);
        if (r >= 0) g_fanout_ok++;
        else { g_fanout_err++; g_last_errno = errno; if (errno == ENOENT) { taps_dirty = 1; } }
    }
}

static int bind_listen(const char *path)
{
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un u;
    int i, ok = -1;
    if (s < 0) { perror("socket listen"); return -1; }
    for (i = 0; i < 5; i++) {
        unlink(path);
        memset(&u, 0, sizeof(u));
        u.sun_family = AF_UNIX;
        strncpy(u.sun_path, path, sizeof(u.sun_path) - 1);
        if (bind(s, (struct sockaddr *)&u, sizeof(u)) == 0) { ok = 0; break; }
        if (errno != EADDRINUSE) break;
        usleep(1500000);
    }
    if (ok < 0) { perror("bind listen"); close(s); return -1; }
    return s;
}

int main(int argc, char **argv)
{
    int i, ls;
    struct sockaddr_un eng_addr;
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--listen"))      listen_path = argv[i + 1];
        else if (!strcmp(argv[i], "--engine")) engine_path = argv[i + 1];
        else if (!strcmp(argv[i], "--tapdir")) tapdir      = argv[i + 1];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    ls = bind_listen(listen_path);
    if (ls < 0) return 1;

    memset(&eng_addr, 0, sizeof(eng_addr));
    eng_addr.sun_family = AF_UNIX;
    strncpy(eng_addr.sun_path, engine_path, sizeof(eng_addr.sun_path) - 1);

    fprintf(stderr, "xaudio-tap-mipns: listen=%s engine=%s tapdir=%s\n",
            listen_path, engine_path, tapdir);

    for (;;) {
        uint8_t buf[MAXD];
        struct pollfd pf = { ls, POLLIN, 0 };
        struct sockaddr_un src;
        socklen_t slen = sizeof(src);
        ssize_t n;
        int r = poll(&pf, 1, 1000);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (r == 0) {
            static unsigned idle_ticks;
            if (++idle_ticks % 5 == 0)
                fprintf(stderr, "stat: cli=%u eng=%u fanout_calls=%u ok=%u err=%u(ntaps=%d,lasterrno=%d)\n",
                        g_cli_pkts, g_eng_pkts, g_fanout_calls, g_fanout_ok, g_fanout_err, n_taps, g_last_errno);
            continue;
        }
        memset(&src, 0, sizeof(src));
        n = recvfrom(ls, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n <= 0) continue;
        if (slen > (socklen_t)offsetof(struct sockaddr_un, sun_path) &&
            src.sun_path[0] != '\0' &&
            strncmp(src.sun_path, engine_path, sizeof(eng_addr.sun_path) - 1) == 0) {
            /* 引擎→客户端：fanout（持续送流）+ 回送最后请求方 */
            g_eng_pkts++;
            taps_fanout(buf, (size_t)n);
            if (have_client) {
                sendto(ls, buf, (size_t)n, MSG_NOSIGNAL, &client_addr, client_len);
            }
        } else {
            /* 客户端→引擎：记录请求方，转发真引擎 */
            g_cli_pkts++;
            memcpy(&client_addr, &src, sizeof(client_addr));
            if (client_len == 0 || !have_client) { have_client = 1; }
            client_len = slen;
            sendto(ls, buf, (size_t)n, MSG_NOSIGNAL,
                   (struct sockaddr *)&eng_addr, sizeof(eng_addr));
        }
    }
    unlink(listen_path);
    return 0;
}
