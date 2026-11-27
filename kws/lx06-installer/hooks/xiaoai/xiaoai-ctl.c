/* xiaoai-ctl — LX06 官方语音链路一次性控制器（V30 新方案）
 *
 * 调用即退出，无常驻进程、无周期循环任务。全部走固件原生接口：
 *
 *   kws on|off     官方「小爱同学」唤醒开关
 *                  DOWN type 9/8 → /tmp/mipns/usock/speech.usock
 *                  （格式与旧 xaudio-tap kws_arm 一致：
 *                   outer{f1=1 DOWNWARD, f3=inner{f1=type}}）
 *   audio open     开 voip 会话（pnshelper 事件 7,8,unlink,mkfifo,2,open,5）
 *                  并 fork 一个 holder 保持 fifo 读端（会话保持开关，非循环）
 *   audio close    事件 3 关会话 + 结束 holder
 *   audio status   holder 存活 + 500ms 数据探测
 *   noplay on|off  官方回复静音开关（事件 7/8），门脚本用
 *   wake           虚拟唤醒（pnshelper src=2/mdspeech 通道；事件号经
 *                  环境变量 WAKE_EV 指定，S7 实测捕获后定死默认值）
 *
 * 原理依据（S0 勘察实证）：
 *   - pns(mipns-xiaomi) 是常驻 KWS/VPM 引擎，绑定 /tmp/mipns/usock/speech.usock，
 *     连接 /tmp/mico_aivs_lab/usock/speech.usock（mico 原生路径，本方案不改绑）。
 *   - voip 会话期间 pns 把原生 VPM 处理后的 16k s16 mono PCM 写进
 *     /tmp/mico-voip-audio.fifo（960B/帧 = 30ms）。pns 启动时持有 fifo 的
 *     旧 inode（unlink 后显示 deleted），新读端须经 /proc/<pns>/fd/N 接入
 *     或走 7/8/2/5 重开序列（与 vpm-feed 同款，字节级照搬）。
 *   - 事件语义：5=会话开始 flag:1；3=会话结束 flag:0；8=mute:0；7=mute:1
 *     （wakeup mute，官方回复静音）；2=重开 fifo 读端。
 *
 * 静态单文件，无第三方依赖。
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
#include <sys/wait.h>
#include <signal.h>

#define PNS_FIFO      "/tmp/mico-voip-audio.fifo"
#define PNS_SOCK      "/tmp/mipns/usock/speech.usock"
#define MICO_PATH     "/tmp/mico_aivs_lab/usock/speech.usock"
#define CTL_DIR       "/tmp/xiaoai-ctl"
#define CTL_SOCK      "/tmp/xiaoai-ctl/usock/speech.usock"
#define HOLDER_PID    "/tmp/xiaoai-ctl/holder.pid"
#define WAKE_EV_DEFAULT "2"

/* ---- pnshelper 事件（ubus，src=3 "other"）---- */
static void ensure_ctl_dir(void)
{
    mkdir(CTL_DIR, 0755);
    mkdir("/tmp/xiaoai-ctl/usock", 0755);
}
static int pns_event(int ev)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ubus call pnshelper event_notify '{\"src\":3,\"event\":%d}' "
             ">/dev/null 2>&1", ev);
    return system(cmd) == 0 ? 0 : -1;
}
static int pns_event_src2(int ev)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ubus call pnshelper event_notify '{\"src\":2,\"event\":%d}' "
             ">/dev/null 2>&1", ev);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---- protobuf 编码（照搬 xaudio-tap.c）---- */
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

/* ---- 找 pns PID（/proc 下 cmdline 含 mipns-xiaomi 的进程）---- */
static int pns_pid(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    int pid = 0;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char cpath[64], buf[128];
        int cfd, n;
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        snprintf(cpath, sizeof(cpath), "/proc/%s/cmdline", de->d_name);
        cfd = open(cpath, O_RDONLY);
        if (cfd < 0) continue;
        n = (int)read(cfd, buf, sizeof(buf) - 1);
        close(cfd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strstr(buf, "mipns-xiaomi")) { pid = atoi(de->d_name); break; }
    }
    closedir(d);
    return pid;
}

/* ---- pns 当前持有的 fifo 写端 fd（/proc/<pns>/fd/N 读入）---- */
static int attach_pns_fifo(void)
{
    struct dirent *de;
    DIR *d;
    char fddir[64];
    int pid = pns_pid();
    if (pid <= 0) return -1;
    snprintf(fddir, sizeof(fddir), "/proc/%d/fd", pid);
    d = opendir(fddir);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char path[96], link[160];
        ssize_t n;
        int fd;
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/%d/fd/%s", pid, de->d_name);
        n = readlink(path, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "mico-voip-audio.fifo")) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK);
        closedir(d);
        return fd;
    }
    closedir(d);
    return -1;
}

/* 非阻塞探测：ms 内能否读到数据（会话是否真在出流） */
static int probe_stream(int fd, int ms)
{
    uint8_t c[256];
    int waited = 0;
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if (r > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(fd, c, sizeof(c));
            if (n > 0) return 1;
        }
        waited += 200;
        if (waited >= ms) break;
    }
    return 0;
}

/* ---- 完整开会话序列（照搬 vpm-feed voip_session，字节级一致）----
 * 7/8 放开旧 fifo → 新建 → 2 让 pns open 新 fifo（阻塞等读端）
 * → 我方 open（O_RDWR 解除阻塞）→ 5 会话开始。
 * 返回我方持有的读端 fd（磁盘 fifo O_RDWR）；失败 -1。 */
static int session_open(void)
{
    int fd;
    pns_event(7);
    pns_event(8);
    unlink(PNS_FIFO);
    if (mkfifo(PNS_FIFO, 0666) != 0 && errno != EEXIST) return -1;
    chmod(PNS_FIFO, 0666);
    if (pns_event(2) != 0) return -1;
    usleep(300000);
    fd = open(PNS_FIFO, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    usleep(200000);
    if (pns_event(5) != 0) { close(fd); return -1; }
    return fd;
}

/* ---- kws：DOWN 9/8 注入 pns ---- */
static int cmd_kws(const char *arg)
{
    int on = 0, off = 0, maybe = 0;
    int fd, io = 0, oo = 0;
    uint8_t inner[16], outer[32];
    struct sockaddr_un a;
    if (!strcmp(arg, "on")) on = 1;
    else if (!strcmp(arg, "off")) off = 1;
    else if (!strcmp(arg, "maybe")) maybe = 1;
    else { fprintf(stderr, "usage: xiaoai-ctl kws on|off|maybe\n"); return 2; }
    if (!on && !off && !maybe) return 0;
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    if (!on && !off) { close(fd); return 0; }
    ensure_ctl_dir();
    unlink(CTL_SOCK);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", CTL_SOCK);
    /* 绑定自己的命名 socket 作为源地址（V27 实证：mipns 拒收 unnamed
     * datagram；源地址是否为 mico 原生路径待 S2 验证，不通则走
     * pnshelper 事件回退）。 */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "xiaoai-ctl: kws bind: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    io = 0; oo = 0;
    enc_fv(inner, &io, 1, (unsigned long long)(on ? 9 : 8));
    enc_fv(outer, &oo, 1, 1);                 /* DOWNWARD */
    enc_fb(outer, &oo, 3, inner, (size_t)io); /* body */
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", PNS_SOCK);
    if (sendto(fd, outer, (size_t)oo, MSG_NOSIGNAL,
               (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "xiaoai-ctl: kws send type=%d: %s\n",
                on ? 9 : 8, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    printf("kws %s: DOWN type=%d sent\n", on ? "on" : "off", on ? 9 : 8);
    return 0;
}

/* ---- expect：DOWN EXPECT_SPEECH (type 3) 注入 pns ----
 * 实证（12:55）：voip 会话空闲时 VPM 流是突发式（duty 22-32%），
 * 官方对话中才连续 —— 门控在 speech.usock 捕获层（pns 处于
 * expect-speech 状态才持续转发）。格式照搬 speech.rs send_downward：
 *   outer{f1=1 DOWNWARD, f3=downward}
 *   downward{f1=3 EXPECT_SPEECH, f2=dialog_id(32hex), f4={f2:false}}
 * dialog_id 固定存 /tmp/xiaoai-ctl/dialog_id（一个会话一个 id）。
 * xiaoai-audio --keep 周期调用（10s）保持流热态。 */
static int cmd_expect(void)
{
    static const char hexd[] = "0123456789abcdef";
    char dlg[33] = {0};
    char dpath[64];
    FILE *f;
    int i, fd, io = 0, oo = 0, bo = 0;
    uint8_t inner[96], outer[160], expect[4];
    struct sockaddr_un a;
    snprintf(dpath, sizeof(dpath), "%s/dialog_id", CTL_DIR);
    f = fopen(dpath, "r");
    if (f) {
        size_t n = fread(dlg, 1, 32, f);
        fclose(f);
        if (n != 32) dlg[0] = 0;
    }
    if (!dlg[0]) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (i = 0; i < 32; i++) dlg[i] = hexd[rand() % 16];
        dlg[32] = 0;
        ensure_ctl_dir();
        f = fopen(dpath, "w");
        if (f) { fwrite(dlg, 1, 32, f); fclose(f); }
    }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    ensure_ctl_dir();
    unlink(CTL_SOCK);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", CTL_SOCK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "xiaoai-ctl: expect bind: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    enc_fv(expect, &bo, 2, 0);            /* {f2: false} */
    enc_fv(inner, &io, 1, 3);             /* EXPECT_SPEECH */
    enc_fb(inner, &io, 2, (const uint8_t *)dlg, 32);
    enc_fb(inner, &io, 4, expect, (size_t)bo);
    enc_fv(outer, &oo, 1, 1);             /* DOWNWARD */
    enc_fb(outer, &oo, 3, inner, (size_t)io);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", PNS_SOCK);
    if (sendto(fd, outer, (size_t)oo, MSG_NOSIGNAL,
               (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "xiaoai-ctl: expect send: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

/* ---- holder：继承读端 fd 后 pause，被 SIGTERM 杀掉 ---- */
static int write_pidfile(void)
{
    FILE *f = fopen(HOLDER_PID, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}
static void on_term(int sig) { (void)sig; _exit(0); }

static int cmd_audio_open(void)
{
    int fd = attach_pns_fifo();
    int opened_by_us = 0;
    if (fd >= 0) {
        if (probe_stream(fd, 1000)) {
            printf("audio open: session already active (pns pid %d)\n", pns_pid());
            /* 会话已开着：直接拿 pns fd 维持即可 */
        } else {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        fd = session_open();
        opened_by_us = 1;
        if (fd < 0) {
            fprintf(stderr, "xiaoai-ctl: audio open: session_open failed\n");
            return 1;
        }
    }
    /* fork holder：持有读端，使 pns 的写端永不 EPIPE */
    pid_t p = fork();
    if (p < 0) {
        fprintf(stderr, "xiaoai-ctl: fork: %s\n", strerror(errno));
        close(fd);
        if (opened_by_us) pns_event(3);
        return 1;
    }
    if (p == 0) {
        /* child */
        setsid();
        signal(SIGTERM, on_term);
        signal(SIGINT, on_term);
        signal(SIGHUP, SIG_IGN);
        ensure_ctl_dir();
        if (write_pidfile() < 0) _exit(1);
        for (;;) pause();
    }
    usleep(50000); /* child 写 pidfile 后 pause；短等同步即可，不可 waitpid
                    * （child 长活到 SIGTERM） */
    printf("audio open: holder pid %d (pns pid %d)%s\n",
           (int)p, pns_pid(), opened_by_us ? " [opened]" : "");
    return 0;
}

static int cmd_audio_close(void)
{
    int holder = 0;
    FILE *f = fopen(HOLDER_PID, "r");
    if (f) {
        if (fscanf(f, "%d", &holder) != 1) holder = 0;
        fclose(f);
    }
    unlink(HOLDER_PID);
    int event_failed = pns_event(3) != 0;
    if (event_failed) {
        fprintf(stderr, "xiaoai-ctl: audio close: event 3 failed\n");
    }
    if (holder > 0) {
        kill(holder, SIGTERM);
        usleep(100000);
        if (kill(holder, 0) == 0) kill(holder, SIGKILL);
    }
    printf("audio close: event 3 + holder %d\n", holder);
    return event_failed ? 1 : 0;
}

static int cmd_audio_status(void)
{
    int holder = 0;
    int fd, active = 0;
    FILE *f = fopen(HOLDER_PID, "r");
    if (f) {
        if (fscanf(f, "%d", &holder) != 1) holder = 0;
        fclose(f);
    }
    if (holder > 0) {
        if (kill(holder, 0) != 0) holder = 0;
    }
    fd = attach_pns_fifo();
    if (fd >= 0) {
        active = probe_stream(fd, 500);
        close(fd);
    }
    printf("holder: %s, stream: %s (pns pid %d)\n",
           holder ? "alive" : "none", active ? "active" : "idle", pns_pid());
    return 0;
}

/* ---- noplay：官方回复静音（门脚本用）---- */
static int cmd_noplay(const char *arg)
{
    if (!strcmp(arg, "on")) {
        if (pns_event(7) != 0) { fprintf(stderr, "noplay on: event 7 failed\n"); return 1; }
        printf("noplay on: official reply muted (event 7)\n");
    } else if (!strcmp(arg, "off")) {
        if (pns_event(8) != 0) { fprintf(stderr, "noplay off: event 8 failed\n"); return 1; }
        printf("noplay off: unmuted (event 8)\n");
    } else {
        fprintf(stderr, "usage: xiaoai-ctl noplay on|off\n");
        return 2;
    }
    return 0;
}

/* ---- 虚拟唤醒：mdspeech 通道（事件号 S7 实测后定死）---- */
static int cmd_wake(void)
{
    const char *ev = getenv("WAKE_EV");
    int n = ev ? atoi(ev) : atoi(WAKE_EV_DEFAULT);
    if (n <= 0) { fprintf(stderr, "wake: WAKE_EV not set\n"); return 2; }
    if (pns_event_src2(n) != 0) {
        fprintf(stderr, "wake: src2 event %d failed\n", n);
        return 1;
    }
    printf("wake: pnshelper src=2 event=%d sent\n", n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: xiaoai-ctl <cmd>\n"
            "  kws on|off|maybe   官方唤醒开关（DOWN 9/8）\n"
            "  audio open|close|status   voip 会话开关\n"
            "  noplay on|off       官方回复静音\n"
            "  wake                虚拟唤醒（WAKE_EV）\n"
            "  expect              expect-speech 保活（DOWN type 3）\n");
        return 2;
    }
    if (!strcmp(argv[1], "hold")) return 0; /* 内部保留 */
    if (!strcmp(argv[1], "kws")) {
        if (argc < 3) return cmd_kws("maybe");
        return cmd_kws(argv[2]);
    }
    if (!strcmp(argv[1], "audio")) {
        if (argc < 3) return 2;
        if (!strcmp(argv[2], "open")) return cmd_audio_open();
        if (!strcmp(argv[2], "close")) return cmd_audio_close();
        if (!strcmp(argv[2], "status")) return cmd_audio_status();
        return 2;
    }
    if (!strcmp(argv[1], "noplay")) {
        if (argc < 3) return 2;
        return cmd_noplay(argv[2]);
    }
    if (!strcmp(argv[1], "wake")) return cmd_wake();
    if (!strcmp(argv[1], "expect")) return cmd_expect();
    return 2;
}
