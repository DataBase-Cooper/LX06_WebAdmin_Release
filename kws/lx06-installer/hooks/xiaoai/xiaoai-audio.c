/* xiaoai-audio — LX06 原生 VPM 16k PCM 一次性读取器（V30 新方案）
 *
 *   xiaoai-audio [--dur N] [-o FILE] [--keep] [-f s16|s32]
 *
 * 输出：原生 VPM 处理后的 16 kHz 单声道裸流（stdout 或 -o 文件）。
 *   -f s16（默认）: s16le —— 录音 wav / 自己的 ASR
 *   -f s32:        s16le→s32le（sample<<16，与旧 tap 流同规格）——
 *                  wakeword_template detect / 旧消费端
 * 数据面 = 固件内置语音管线（降噪/AEC/波束成形/VAD/播放时录音），
 * 与官方云对话听到的是同一路流（pns 写 /tmp/mico-voip-audio.fifo 的
 * 960B 帧 = 480 samples = 30ms）。
 *
 * 生命周期（调用即退出，无周期循环）：
 *   1. attach pns 当前持有的 fifo fd（/proc/<pns>/fd/N，pns 可能持有
 *      unlink 前的旧 inode —— V28 实证）；
 *   2. 1.5s 无数据 = 会话未开 → 走 7/8/unlink/mkfifo/2/open/5 开一次
 *      voip 会话（pnshelper 原生事件，与 vpm-feed 字节级一致），
 *      之后 re-attach pns 新 fd；
 *   3. 读循环：--dur N 到点停，或 SIGTERM 停；EOF 时（pns 关 fifo）
 *      在 --dur 未到期前最多重试 3 次开会话；
 *   4. 退出：本次会话由我方开的且非 --keep → 事件 3 关会话，
 *      回出厂静默态。
 *
 * 用法示例：
 *   xiaoai-audio --dur 5 -o /tmp/my.wav   # 录 5 秒
 *   xiaoai-audio --dur 3 | my_local_asr   # 喂自己的 ASR
 *   xiaoai-audio --keep --dur 30          # 借已开会话（localwake 持有时）
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
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define PNS_FIFO "/tmp/mico-voip-audio.fifo"
#define FRAME    960 /* 480 samples, 30ms, 16k S16 mono */

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* ---- pnshelper 事件（与 xiaoai-ctl 同源）---- */
static int pns_event(int ev)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "ubus call pnshelper event_notify '{\"src\":3,\"event\":%d}' "
             ">/dev/null 2>&1", ev);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---- 找 pns PID ---- */
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

/* ---- attach pns 的 fifo 写端（O_RDONLY 经 /proc fd）---- */
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

/* 非阻塞探测：ms 内能否读到数据 */
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

/* ---- 完整开会话序列（照搬 vpm-feed voip_session）---- */
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

/* 建立可读流：先 attach（已有会话），1.5s 无流才开会话。
 * opened_by_us 出参 = 本次调用是否开了会话（决定退出时是否关）。 */
static int open_stream(int *opened_by_us)
{
    int fd = attach_pns_fifo();
    *opened_by_us = 0;
    if (fd >= 0) {
        if (probe_stream(fd, 1500)) {
            fprintf(stderr, "xiaoai-audio: attached pns session (pid %d)\n",
                    pns_pid());
            return fd;
        }
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        fd = session_open();
        *opened_by_us = 1;
        if (fd < 0) {
            fprintf(stderr, "xiaoai-audio: session_open failed\n");
            return -1;
        }
        fprintf(stderr, "xiaoai-audio: session opened by us\n");
    }
    return fd;
}

int main(int argc, char **argv)
{
    int dur = 0;          /* 0 = 直到 SIGTERM */
    int keep = 0;
    int fmt_s32 = 0;      /* 0=s16le（默认） 1=s32le（<<16） */
    const char *out = NULL;
    int ofd = -1;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dur") && i + 1 < argc) {
            dur = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--keep")) {
            keep = 1;
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            if (!strcmp(argv[++i], "s32")) fmt_s32 = 1;
            else if (!strcmp(argv[i], "s16")) fmt_s32 = 0;
            else { fprintf(stderr, "xiaoai-audio: -f s16|s32\n"); return 2; }
        } else {
            fprintf(stderr,
                "usage: xiaoai-audio [--dur N] [-o FILE] [--keep] [-f s16|s32]\n");
            return 2;
        }
    }
    if (out) {
        ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) {
            fprintf(stderr, "xiaoai-audio: -o %s: %s\n", out, strerror(errno));
            return 1;
        }
    }
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGPIPE, SIG_IGN);
    /* V42.1: OOM 免死（118MB 整机，OOM 风暴曾杀掉检测管道旁观者）
     * + stdout EPIPE 自杀：下游 wakeword_template 死亡后，本泵若不退，
     * watch-ww.sh 的管道重启循环被卡（等管道全员退出）→ 整链僵死
     * （10-26 11:2x 实证：template 死、泵活、无自动重启、无检测）。 */
    {
        FILE *oomf = fopen("/proc/self/oom_score_adj", "w");
        if (oomf) { fputs("-1000", oomf); fclose(oomf); }
    }

    {
        int opened_by_us = 0;
        int fd = open_stream(&opened_by_us);
        if (fd < 0) return 1;
        /* 若我方开会话，session_open 返回的是磁盘 fifo O_RDWR fd；
         * 再 attach pns 真实 fd（同一 inode 或新 inode 均可读）。 */
        if (opened_by_us) {
            int afd = attach_pns_fifo();
            if (afd >= 0) {
                close(fd);
                fd = afd;
            }
        }
        {
            uint8_t buf[FRAME];
            uint8_t outbuf[4 * FRAME];
            time_t t0 = time(NULL);
            time_t last_data = t0;
            /* V40: gap-fill 输出时间轴（monotonic ms）。pns 空闲(asr=0)节流
             * 丢块(~27%): 10-26 01:38 实测 513 块/14s=36.6/s vs 满流 50/s。
             * 丢块把 utterance 时间压缩(1.32s→1.08s) → embedding 帧 7→4 →
             * DTW ratio 门(0.55) 全拒 → 无 WAKE。每 30ms 无输出补一帧 30ms
             * 静默(960B)恢复实时时间轴，官方云 ASR 同受益(连续流)。 */
            struct timespec _ts0;
            clock_gettime(CLOCK_MONOTONIC, &_ts0);
            long long last_out_ms = (long long)_ts0.tv_sec * 1000 + _ts0.tv_nsec / 1000000;
            /* 一次性（--dur）：流丢 3 次后退出；常驻（无 --dur，watch-ww
             * 管道用）：无限重试自恢复，procd 是最后兜底。 */
            int retries = dur > 0 ? 3 : -1;
            for (;;) {
                if (g_stop) break;
                if (dur > 0 && (int)(time(NULL) - t0) >= dur) break;
                if (fd >= 0) {
                    ssize_t n = read(fd, buf, sizeof(buf));
                    if (n > 0) {
                        last_data = time(NULL);
                        {
                            struct timespec ts;
                            clock_gettime(CLOCK_MONOTONIC, &ts);
                            last_out_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                        }
                        if (fmt_s32) {
                            /* s16le → s32le（sample<<16，与旧 tap 流同规格）。
                             * n 不足 2 的尾部丢帧（VPM 帧恒为 960B 偶数）。 */
                            int sn = (int)(n / 2) * 2;
                            int s16n, o = 0;
                            for (s16n = 0; s16n + 1 < sn; s16n += 2) {
                                int16_t s16;
                                int32_t s32;
                                memcpy(&s16, buf + s16n, 2);
                                s32 = (int32_t)s16 << 16;
                                memcpy(outbuf + o, &s32, 4);
                                o += 4;
                            }
                            if (ofd >= 0) {
                                ssize_t w = write(ofd, outbuf, (size_t)o);
                                (void)w;
                            } else {
                                ssize_t w = write(STDOUT_FILENO, outbuf, (size_t)o);
                                if (w < 0 && errno == EPIPE) g_stop = 1; /* V42.1: 下游已死→退出让 watch-ww 重启整链 */
                            }
                        } else if (ofd >= 0) {
                            ssize_t w = write(ofd, buf, (size_t)n);
                            (void)w;
                        } else {
                            ssize_t w = write(STDOUT_FILENO, buf, (size_t)n);
                            if (w < 0 && errno == EPIPE) g_stop = 1; /* V42.1 */
                        }
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                        struct pollfd p = { fd, POLLIN, 0 };
                        poll(&p, 1, 10);
                        /* V40 gap-fill：流间隙补 30ms 静默帧，恢复实时时间轴
                         * （见 last_out_ms 处注释；防丢块压缩 utterance）。
                         * 追赶式 +=30：重开/卡顿后的真实空白如实补齐。 */
                        for (;;) {
                            struct timespec ts;
                            clock_gettime(CLOCK_MONOTONIC, &ts);
                            long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
                            if (now_ms - last_out_ms < 30) break;
                            static const uint8_t silence[FRAME];
                            ssize_t w = write(ofd >= 0 ? ofd : STDOUT_FILENO,
                                              silence, sizeof(silence));
                            if (w < 0 && errno == EPIPE && ofd < 0) g_stop = 1; /* V42.1 */
                            (void)w;
                            last_out_ms += 30;
                        }
                        /* 看门狗：空闲态 VPM 流是突发式（~400ms 突发 +
                         * ~1-4s 间隙，12:25 实证 duty 22-32%），对话中才
                         * 连续。15s 无数据 = 会话已死（pns fifo 写端不关、
                         * 无 EOF 可感知 —— V28 实证）。重走开会话序列。
                         * 阈值 5s 会误判间隙为死会话 → 重开循环 → 压制
                         * 官方 KWS（12:48 实证）。 */
                        if (time(NULL) - last_data >= 15) {
                            int o2 = 0;
                            int nfd = open_stream(&o2);
                            if (nfd >= 0) {
                                close(fd);
                                fd = nfd;
                                if (o2) {
                                    opened_by_us = 1;
                                    fprintf(stderr,
                                        "xiaoai-audio: watchdog: session reopened\n");
                                }
                            }
                            last_data = time(NULL);
                        }
                        continue;
                    }
                    /* EOF 或硬错：pns 关了 fifo（会话死）→ 重试开会话 */
                    close(fd);
                    fd = -1;
                }
                if (retries >= 0 && --retries <= 0) break;
                sleep(1);
                fprintf(stderr, "xiaoai-audio: stream lost, reopening\n");
                {
                    int o2 = 0;
                    fd = open_stream(&o2);
                }
            }
            if (fd >= 0) close(fd);
            if (opened_by_us && !keep) {
                pns_event(3);
                fprintf(stderr, "xiaoai-audio: session closed (event 3)\n");
            }
        }
    }
    if (ofd >= 0) close(ofd);
    if (out) fprintf(stderr, "xiaoai-audio: done (%s)\n", out);
    return 0;
}
