/* LX06 噪音抑制 tap 代理（V28 双路由）· armhf 静态 · root
 *
 * 角色：坐在官方 mipns-xiaomi 与「mic 代理」/「真实官方引擎」之间，双向透传
 * 原生 unix dgram socket /tmp/mico_aivs_lab/usock/speech.usock 上的
 * 全部 datagram（mipns 原生 protobuf 报文），并按路由把上行报文送到
 * 两个目的端之一：
 *
 *   mipns-xiaomi  ⇄  [xaudio-tap]  ⇄  非官方: xiaoai-mic (agent.usock, speech 协议)
 *                        ⇅  路由切换
 *                 ⇄  官方帧: LAB2 speech.usock
 *                    官方登记/TTS: common.usock（协议不同，不能收 speech 帧）
 *
 * 路由（由 webd/tap 文件控制）：
 *   --route  /tmp/xiaoai-agent/route          临时覆盖（120s TTL，按 mtime 过期）
 *   --rdef   /tmp/xiaoai-agent/route.default  持久默认
 *   "official" → 官方唤醒 / 链到官方的本地词。登记和 TTS 在 common.usock，
 *                但 speech 帧不能原样送进去（mico: common message unpack failed）。
 *                帧仍送 LAB2 speech.usock，直到有 common 协议封装。
 *   "mic"      → 非官方唤醒：上行送到 speech 协议端（xiaoai-mic）。
 *   pns 自身「小爱同学」不写临时文件 → 走默认 official。
 *
 * 同时把每一条上行报文（mipns→目的端 方向，含全部音频帧）复制 fan-out
 * 到 /tmp/xiaoai-agent/taps/*.usock 下的每个消费端 socket：
 *   - kws.usock  本地唤醒词 KWS 检测（watch-ww.sh，-f s32）
 *   - rec.usock  唤醒词录音（ww-record.sh，-f s32）
 *   - xz.usock / db.usock 等  小智/豆包等脚本自绑（-f s16 或 s32）
 * 消费端 = xaudio-tap-read --name <n>（见其源文件）。
 *
 * 数据面特性（来自 VPM 处理后的 16 kHz mono s16le 流）：
 *   降噪 / 回声消除 AEC / 波束成形，支持播放时录音与连续对话。
 *
 * 生命周期：由 /usr/lib/lx06/xiaoai/start.sh 拉起（先于 mico/pns 恢复），
 * pkill 退出（退出时 unlink 监听 socket）。崩溃/重启由 procd 托管的
 * run.sh 兜底（幂等重接管）。
 *
 * 容错：目的端未就绪时上行 send/sendto ECONNREFUSED/ENOENT → 丢包不崩溃；
 * 消费端 socket 离线（ECONNREFUSED）→ 跳过；tapdir 每 300 包（≈10s）
 * 或收到 ENOENT 时重扫（消费端来去自由，无需重启代理）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>

#define MAXD   (8 * 1024)
#define MAXTAP 8
#define RESCAN_EVERY 60u     /* 约 2s 重扫 tapdir：新 tap（xz 会话麦等）尽快接入；ENOENT 亦触发 */
#define ROUTE_TTL 120 /* 秒：临时路由文件 mtime 超过该值即过期，回落到默认 */

static const char *listen_path = "/tmp/mico_aivs_lab/usock/speech.usock";
static const char *peer_path   = "/tmp/xiaoai-agent/agent.usock";
static const char *tapdir      = "/tmp/xiaoai-agent/taps";
/* 目的端 A（mic）转发 socket 必须绑定路径名：xiaoai-mic 上游要求每个上行
 * datagram 的源地址 as_pathname() 非空，否则报
 * "mipns used an unnamed Unix datagram socket" 后 Fatal 退出（V27 实证）。
 * 下行 reply 也发往该路径，tap 再转送 mipns 的真实源地址。 */
static const char *client_path = "/tmp/xiaoai-agent/tap2agent.usock";
/* 官方目的端。common.usock 不能收 speech 帧（mico 会 common message unpack failed）。
 * 官方登记/TTS 走 common.usock，但上行 speech 帧仍须送到 LAB2 speech.usock，
 * 直到有 common 协议封装。不要把 lab_path 改成 common.usock。 */
static const char *lab_path    = "/tmp/mico_aivs_lab2/usock/speech.usock";
static const char *labclient   = "/tmp/xiaoai-agent/tap2lab.usock";
static const char *route_path  = "/tmp/xiaoai-agent/route";
static const char *rdef_path   = "/tmp/xiaoai-agent/route.default";

static struct sockaddr_un mipns_addr; /* 上行报文的源地址（mipns 绑定路径），下行回送目标 */
static char g_mipns_path[108];
static int have_mipns = 0;

/* ---- 路由状态（0=mic 目的端A，1=official 目的端B） ---- */
static int route_official = 0;

/* 读路由文件并修剪尾部空白（start.sh 用 echo 写入，带 \n） */
static int route_readval(const char *path)
{
    char c[16] = {0};
    int f = open(path, O_RDONLY);
    ssize_t rn;
    if (f < 0) return -2;
    rn = read(f, c, sizeof(c) - 1);
    close(f);
    if (rn <= 0) return -2;
    while (rn > 0 && (c[rn - 1] == '\n' || c[rn - 1] == '\r' ||
                      c[rn - 1] == ' ' || c[rn - 1] == '\t')) rn--;
    c[rn] = 0;
    if (!strcmp(c, "official")) return 1;
    if (!strcmp(c, "mic")) return 0;
    return -2;
}

static void route_refresh(void)
{
    /* V44：恒官方路由（无 xiaoai-agent/mic 双路由），不读 route 文件 */
    route_official = 1;
}

/* 官方开关关掉时上行仍走 lab，xiaoai-mic 收不到 REGISTER。
 * 类型 8/9 必须从这条已经连着 mipns 的下行注入。不改 route，不发类型 10。 */
static int g_listen_fd = -1;
static int g_kws_arm = -1;

static void enc_varint(uint8_t *b, int *o, unsigned long long v)
{
    while (v >= 0x80) { b[(*o)++] = (uint8_t)(v | 0x80); v >>= 7; }
    b[(*o)++] = (uint8_t)v;
}
static void enc_fv(uint8_t *b, int *o, int num, unsigned long long v)
{
    enc_varint(b, o, (unsigned long long)num << 3);
    enc_varint(b, o, v);
}
static void enc_fb(uint8_t *b, int *o, int num, const uint8_t *d, size_t l)
{
    enc_varint(b, o, ((unsigned long long)num << 3) | 2);
    enc_varint(b, o, l);
    memcpy(b + *o, d, l);
    *o += (int)l;
}
static int official_xiaoai_on(void)
{
    char c[8] = {0};
    int f = open("/data/wakeword/official_xiaoai", O_RDONLY);
    int n;
    /* 文件不存在或空 = 开。只有明确写成 0 才关「小爱同学」四个字。 */
    if (f < 0) return 1;
    n = (int)read(f, c, sizeof(c) - 1);
    close(f);
    if (n <= 0) return 1;
    while (n > 0 && (c[n - 1] == '\n' || c[n - 1] == '\r' || c[n - 1] == ' ')) n--;
    c[n] = 0;
    return strcmp(c, "0") != 0;
}
static void kws_arm_inject(void)
{
    /* V44：停用唤醒词开关注入（type 8/9 控制）——官方开关状态不被旁路改动；
     * b2 的开关行为后续走官方自身路径。 */
    (void)0;
}

/* ---- tap 消费端缓存（connect 的 dgram socket；离线时 send 报错即跳过） ---- */
static struct tap { char path[160]; int fd; } taps[MAXTAP];
static int n_taps;
static int taps_dirty = 1;
static unsigned tap_pkts;

static void taps_refresh(void)
{
    int i;
    for (i = 0; i < n_taps; i++) {
        if (taps[i].fd >= 0) { close(taps[i].fd); taps[i].fd = -1; }
    }
    n_taps = 0;
    taps_dirty = 0;
    DIR *d = opendir(tapdir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 7 || strcmp(de->d_name + l - 6, ".usock") != 0) continue;
        if (n_taps >= MAXTAP) break;
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) continue;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", tapdir, de->d_name);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); continue; }
        snprintf(taps[n_taps].path, sizeof(taps[n_taps].path),
                 "%s/%s", tapdir, de->d_name);
        taps[n_taps].fd = fd;
        n_taps++;
    }
    closedir(d);
}

static void taps_fanout(const uint8_t *buf, size_t n)
{
    int i;
    if (taps_dirty) taps_refresh();
    else if (++tap_pkts % RESCAN_EVERY == 0) { taps_dirty = 1; taps_refresh(); }
    for (i = 0; i < n_taps; i++) {
        ssize_t r = send(taps[i].fd, buf, n, MSG_NOSIGNAL);
        if (r < 0 && errno == ENOENT) { taps_dirty = 1; } /* 文件消失 → 下包重扫 */
        /* ECONNREFUSED：消费端已退出（socket 文件残留）→ 跳过，等下次重扫 */
    }
}

/* mico stop 会拆掉 /tmp/mico_aivs_lab。目录不在时 bind 报 ENOENT，
 * start.sh 回滚，页面先「tap 缺失」再「未启用」。先把父目录建回来。 */
static void mkdir_parents(const char *path)
{
    char dir[108];
    char *slash;
    size_t n;

    slash = strrchr(path, '/');
    if (!slash || slash == path) return;
    n = (size_t)(slash - path);
    if (n >= sizeof(dir)) return;
    memcpy(dir, path, n);
    dir[n] = 0;
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdir(dir, 0755);
        *slash = '/';
    }
    mkdir(dir, 0755);
}

static int bind_listen(const char *path)
{
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket listen"); return -1; }
    mkdir_parents(path);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    /* V30：retry bind。lazy umount 残留的 mount point 或上一轮未退干净的
     * 旧实例会让 unlink 成功但 bind 仍 EADDRINUSE（V20261024.1 实测
     * tap.log 15 次）→ 旧代码直接 return -1 → tap 退出 → run.sh 40s
     * 一轮重新接管 → 开机重启风暴。重试 5×1.5s 给旧实例/挂载回收时间。 */
    int ok = -1;
    for (int i = 0; i < 5; i++) {
        unlink(path);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0) { ok = 0; break; }
        if (errno != EADDRINUSE) break;
        usleep(1500000);
    }
    if (ok < 0) {
        perror("bind listen");
        close(s);
        return -1;
    }
    return s;
}

int main(int argc, char **argv)
{
    int i, ls, ag, lg;
    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--listen"))      listen_path = argv[i + 1];
        else if (!strcmp(argv[i], "--peer"))   peer_path   = argv[i + 1];
        else if (!strcmp(argv[i], "--tapdir"))   tapdir      = argv[i + 1];
        else if (!strcmp(argv[i], "--clientbind")) client_path = argv[i + 1];
        else if (!strcmp(argv[i], "--labpath"))    lab_path  = argv[i + 1];
        else if (!strcmp(argv[i], "--labclient"))  labclient = argv[i + 1];
        else if (!strcmp(argv[i], "--route"))      route_path = argv[i + 1];
        else if (!strcmp(argv[i], "--rdef"))       rdef_path  = argv[i + 1];
        else { fprintf(stderr, "xaudio-tap: unknown arg %s\n", argv[i]); return 2; }
    }
    ls = bind_listen(listen_path);
    if (ls < 0) return 1;
    g_listen_fd = ls;

    ag = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ag < 0) { perror("socket agent"); return 1; }
    struct sockaddr_un ca;
    memset(&ca, 0, sizeof(ca));
    ca.sun_family = AF_UNIX;
    strncpy(ca.sun_path, client_path, sizeof(ca.sun_path) - 1);
    /* V30：同 bind_listen 的重试（残留 mount/旧实例 EADDRINUSE） */
    int cok = -1;
    for (int i = 0; i < 5; i++) {
        unlink(client_path);
        if (bind(ag, (struct sockaddr *)&ca, sizeof(ca)) == 0) { cok = 0; break; }
        if (errno != EADDRINUSE) break;
        usleep(1500000);
    }
    if (cok < 0) {
        perror("bind client (agent 端转发 socket 需绑定路径名)");
        close(ag);
        return 1;
    }
    struct sockaddr_un pa;
    memset(&pa, 0, sizeof(pa));
    pa.sun_family = AF_UNIX;
    strncpy(pa.sun_path, peer_path, sizeof(pa.sun_path) - 1);
    if (connect(ag, (struct sockaddr *)&pa, sizeof(pa)) < 0) {
        /* V44：无 xiaoai-agent（双路由退役）——peer 缺失不致命，ag 置 -1
         * （poll 对 fd<0 的项按 POSIX 直接忽略；官方路由永不 send(ag)） */
        fprintf(stderr, "xaudio-tap: agent 缺失（%s），单官方模式继续\n", peer_path);
        close(ag);
        ag = -1;
    }

    /* V28 目的端 B：真实引擎（仅 bind 源地址名，不 connect；
     * sendto 显式发往 lab_path。lab 未 bind LAB2 前 ENOENT 丢包）。 */
    lg = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (lg < 0) { perror("socket lab"); return 1; }
    struct sockaddr_un la2;
    memset(&la2, 0, sizeof(la2));
    la2.sun_family = AF_UNIX;
    strncpy(la2.sun_path, labclient, sizeof(la2.sun_path) - 1);
    unlink(labclient);
    if (bind(lg, (struct sockaddr *)&la2, sizeof(la2)) < 0) {
        perror("bind labclient (lab 端转发 socket 需绑定路径名)");
        close(lg);
        return 1;
    }
    struct sockaddr_un lab_addr;
    memset(&lab_addr, 0, sizeof(lab_addr));
    lab_addr.sun_family = AF_UNIX;
    strncpy(lab_addr.sun_path, lab_path, sizeof(lab_addr.sun_path) - 1);

    fprintf(stderr, "xaudio-tap: listen=%s peer=%s client=%s labpath=%s tapdir=%s\n",
            listen_path, peer_path, client_path, lab_path, tapdir);
    route_refresh();

    for (;;) {
        uint8_t buf[MAXD];
        struct pollfd pf[3] = { { ls, POLLIN, 0 }, { ag, POLLIN, 0 }, { lg, POLLIN, 0 } };
        int r = poll(pf, 3, 1000);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (r == 0) { kws_arm_inject(); continue; }
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            socklen_t alen = sizeof(mipns_addr);
            memset(&mipns_addr, 0, sizeof(mipns_addr));
            ssize_t n = recvfrom(ls, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&mipns_addr, &alen);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n > 0) {
                if (mipns_addr.sun_family == AF_UNIX) {
                    if (!have_mipns || strcmp(g_mipns_path, mipns_addr.sun_path) != 0) {
                        have_mipns = 1;
                        g_kws_arm = -1;
                        strncpy(g_mipns_path, mipns_addr.sun_path, sizeof(g_mipns_path) - 1);
                        g_mipns_path[sizeof(g_mipns_path) - 1] = 0;
                    }
                }
                route_refresh();
                taps_fanout(buf, (size_t)n);   /* tap 优先：音频连续性高于目的端 */
                if (route_official) {
                    /* speech 帧只能送 LAB2 speech.usock。
                     * common.usock 是另一套协议，原样送会 unpack failed。 */
                    if (sendto(lg, buf, (size_t)n, MSG_NOSIGNAL,
                               (struct sockaddr *)&lab_addr, sizeof(lab_addr)) < 0) {
                        /* ENOENT: lab 尚未 bind LAB2（重启窗口）→ 丢包下包重试 */
                    }
                } else {
                    if (send(ag, buf, (size_t)n, MSG_NOSIGNAL) < 0) {
                        /* ECONNREFUSED：mic 重启中，丢包（procd 会拉起） */
                    }
                }
            }
        }
        if (pf[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = recv(ag, buf, sizeof(buf), 0);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n > 0 && have_mipns) {
                if (sendto(ls, buf, (size_t)n, MSG_NOSIGNAL,
                           (struct sockaddr *)&mipns_addr, sizeof(mipns_addr)) < 0) {
                    /* EPIPE/ENOTCONN：mipns 侧 socket 瞬断，丢包 */
                }
            }
        }
        if (pf[2].revents & (POLLIN | POLLHUP)) {
            ssize_t n = recv(lg, buf, sizeof(buf), 0);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n > 0 && have_mipns) {
                if (sendto(ls, buf, (size_t)n, MSG_NOSIGNAL,
                           (struct sockaddr *)&mipns_addr, sizeof(mipns_addr)) < 0) {
                    /* EPIPE/ENOTCONN：mipns 侧 socket 瞬断，丢包 */
                }
            }
        }
        {
            static time_t last_arm = 0;
            time_t now = time(NULL);
            if (now != last_arm) {
                last_arm = now;
                kws_arm_inject();
            }
        }
    }
    unlink(listen_path);
    unlink(client_path);
    unlink(labclient);
    for (i = 0; i < n_taps; i++) if (taps[i].fd >= 0) close(taps[i].fd);
    close(ls);
    close(ag);
    close(lg);
    return 0;
}
