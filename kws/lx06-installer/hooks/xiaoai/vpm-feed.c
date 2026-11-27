/* vpm-feed — 把官方 VPM 的 voip 麦流扇出到 tap 消费端。
 *
 * 空闲时 speech.usock 不送 UP_STREAM_TRANSMITTING，xaudio-tap 扇出是 0 字节，
 * 所以唤醒词录音和本地 KWS 都录不到。pnshelper event 2/5 会把已经做过
 * 降噪/AEC 的 16 kHz mono S16 写进 /tmp/mico-voip-audio.fifo。本程序占住
 * 这条流，按 30ms 打成与 xaudio-tap 相同的上行报文，发给
 * /tmp/xiaoai-agent/taps/*.usock。xaudio-tap-read 不用改。
 *
 * 不打开 ALSA（mipns 独占），不做第二套降噪，不发 STOP_CAPTURE。
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "glibc_ni_trap.h"

#define TAPDIR "/tmp/xiaoai-agent/taps"
#define FIFO "/tmp/mico-voip-audio.fifo"
#define FRAME 960 /* 480 samples, 30ms, 16k S16 mono */

static volatile sig_atomic_t g_stop;

static void on_stop(int sig) { (void)sig; g_stop = 1; }

static size_t put_varint(uint8_t *p, unsigned v)
{
    size_t n = 0;
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        p[n++] = b;
    } while (v);
    return n;
}

/* 顶层 field1=0(UPWARD) field2=body; body field1=3(TRANSMITTING) field4=data;
 * data field1=0(pcm16) field2=pcm。与 xaudio-tap-read 的解析对称。 */
static size_t pack_pcm(uint8_t *out, const uint8_t *pcm, size_t pcm_n)
{
    uint8_t data[16 + FRAME];
    uint8_t body[32 + FRAME];
    size_t dn = 0, bn = 0, on = 0;
    data[dn++] = 0x08; data[dn++] = 0x00;
    data[dn++] = 0x12;
    dn += put_varint(data + dn, (unsigned)pcm_n);
    memcpy(data + dn, pcm, pcm_n);
    dn += pcm_n;

    body[bn++] = 0x08; body[bn++] = 0x03;
    body[bn++] = 0x22;
    bn += put_varint(body + bn, (unsigned)dn);
    memcpy(body + bn, data, dn);
    bn += dn;

    out[on++] = 0x08; out[on++] = 0x00;
    out[on++] = 0x12;
    on += put_varint(out + on, (unsigned)bn);
    memcpy(out + on, body, bn);
    return on + bn;
}

static int taps[16];
static int n_taps;

static void taps_close(void)
{
    int i;
    for (i = 0; i < n_taps; i++) close(taps[i]);
    n_taps = 0;
}

static void taps_refresh(void)
{
    DIR *d;
    struct dirent *de;
    taps_close();
    d = opendir(TAPDIR);
    if (!d) return;
    while ((de = readdir(d)) != NULL && n_taps < 16) {
        size_t l = strlen(de->d_name);
        struct sockaddr_un a;
        int fd;
        if (l < 7 || strcmp(de->d_name + l - 6, ".usock") != 0) continue;
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) continue;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", TAPDIR, de->d_name);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            close(fd);
            continue;
        }
        taps[n_taps++] = fd;
    }
    closedir(d);
}

static void fanout(const uint8_t *pkt, size_t n)
{
    int i;
    for (i = 0; i < n_taps; i++) {
        if (send(taps[i], pkt, n, MSG_NOSIGNAL) < 0) {
            /* 对端重绑后下轮 refresh 会重连 */
        }
    }
}

static int pns_event(int ev)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd),
             "ubus call pnshelper event_notify '{\"src\":3,\"event\":%d}' >/dev/null 2>&1",
             ev);
    return system(cmd) == 0 ? 0 : -1;
}

/* 会话 = xaudio 麦 DSP 的电源：voip 会话在 → speech.usock 出流 →
 * tap(rec/kws) 有数据 → 本地 KWS/录音/xiaoai-mic 才听得见（实测 mico
 * 健康+无会话时 tap-read 恒 0B，V32 曾按"mico 健康即 idle"设计导致
 * 本地 KWS 全聋）。故 vpm-feed 常驻保持会话；官方 NLP 与 flag:1 的
 * 竞争由 pns 在官方唤醒启动时自行清 flag 解决（实测 5/5 有回复，
 * 首包至多被拒一次后恢复）。 */
/* pns 重启后仍写它启动时 open 的旧 fifo（unlink 后显示 deleted），
 * 同名 mkfifo 读不到 → 2s dry → 7/8/2/5 事件风暴 → xaudio 每 4s
 * stop/start、voip flag:1 挂死（官方 NLP 回复被 voip filter 拒）。
 * 直接打开 /proc/<pns>/fd/N（同一 inode）即可接上真实流。 */
static int attach_mipns_fifo(void)
{
    struct dirent *de;
    DIR *d;
    char proc[64];
    int pid = 0;
    DIR *pd = opendir("/proc");
    struct dirent *pe;
    if (pd) {
        while ((pe = readdir(pd)) != NULL) {
            char cpath[64], buf[80];
            int cfd, n;
            if (pe->d_name[0] < '1' || pe->d_name[0] > '9') continue;
            snprintf(cpath, sizeof(cpath), "/proc/%s/cmdline", pe->d_name);
            cfd = open(cpath, O_RDONLY);
            if (cfd < 0) continue;
            n = (int)read(cfd, buf, sizeof(buf) - 1);
            close(cfd);
            if (n <= 0) continue;
            buf[n] = 0;
            if (strstr(buf, "mipns-xiaomi")) { pid = atoi(pe->d_name); break; }
        }
        closedir(pd);
    }
    if (pid <= 0) return -1;
    snprintf(proc, sizeof(proc), "/proc/%d/fd", pid);
    d = opendir(proc);
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
        if (fd >= 0) {
            pns_event(5);
            fprintf(stderr, "vpm-feed: attached mipns fd %s\n", path);
        }
        return fd;
    }
    closedir(d);
    return -1;
}
/* 2 秒没字节就整段重开：7/8 放开旧 fifo，新建，再 2 和 5。不要 attach 旧 fd。 */
static int voip_session(void)
{
    int fd;
    pns_event(7);
    pns_event(8);
    unlink(FIFO);
    if (mkfifo(FIFO, 0666) != 0 && errno != EEXIST) return -1;
    if (pns_event(2) != 0) return -1;
    usleep(300000);
    fd = open(FIFO, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    usleep(200000);
    if (pns_event(5) != 0) {
        close(fd);
        return -1;
    }
    taps_refresh();
    return fd;
}

static int open_stream(void)
{
    /* 优先 attach pns 正在写的 fifo fd（pns 先于本程序启动时唯一正确姿势；
     * 直接 voip_session 的 unlink+mkfifo 会让 pns 继续写已删除 inode → 干等） */
    int fd = attach_mipns_fifo();
    if (fd >= 0)
        return fd;
    return voip_session();
}

int main(void)
{
    int fd, kick = 0;
    uint8_t acc[FRAME];
    size_t got = 0;
    time_t last_byte, last_reopen, last_tap, last_kick;
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGPIPE, SIG_IGN);
    mkdir(TAPDIR, 0755);
    fd = open_stream();
    if (fd < 0)
        fprintf(stderr, "vpm-feed: waiting for voip session (retry 5s)\n");
    last_byte = time(NULL);
    last_reopen = last_byte;
    last_tap = 0;
    last_kick = 0;
    if (fd >= 0)
        fprintf(stderr, "vpm-feed: streaming\n");
    while (!g_stop) {
        uint8_t tmp[FRAME];
        ssize_t n;
        if (fd < 0) {
            if (time(NULL) - last_reopen < 5) {
                sleep(1);
                continue;
            }
            last_reopen = time(NULL);
            fd = open_stream();
            if (fd < 0) continue;
            last_byte = time(NULL);
            fprintf(stderr, "vpm-feed: reopened\n");
        }
        n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                /* 2 秒没字节：先 re-attach pns 真实 fifo fd（免事件风暴）；
                 * attach 不到才整段重开：7/8 放开旧 fifo，新建，再 2 和 5。 */
                if (time(NULL) - last_byte >= 2 && time(NULL) - last_reopen >= 2) {
                    close(fd);
                    fd = attach_mipns_fifo();
                    if (fd >= 0) {
                        fprintf(stderr, "vpm-feed: dry 2s, re-attached\n");
                        last_byte = time(NULL);
                        last_reopen = last_byte;
                        continue;
                    }
                    fd = -1;
                    last_reopen = time(NULL);
                    fprintf(stderr, "vpm-feed: dry 2s, reopen\n");
                    continue;
                } else {
                    if (time(NULL) - last_tap >= 1) {
                        taps_refresh();
                        last_tap = time(NULL);
                    }
                    usleep(5000);
                }
                continue;
            }
            break;
        }
        if (n == 0) {
            usleep(5000);
            continue;
        }
        last_byte = time(NULL);
        size_t off = 0;
        while (off < (size_t)n) {
            size_t c = FRAME - got;
            if (c > (size_t)n - off) c = (size_t)n - off;
            memcpy(acc + got, tmp + off, c);
            got += c;
            off += c;
            if (got == FRAME) {
                uint8_t pkt[32 + FRAME];
                size_t pn = pack_pcm(pkt, acc, FRAME);
                if ((kick++ % 30) == 0) taps_refresh();
                fanout(pkt, pn);
                got = 0;
            }
        }
    }
    taps_close();
    if (fd >= 0) close(fd);
    return 0;
}
