/* pcmtee.c v14 — LD_PRELOAD tee on engine's ALSA capture (snd_pcm_readi)
 * V44: engine sole-opens /dev/snd/pcmC0D3c (S16_LE 8ch@48k RW_INTERLEAVED), readi path.
 * Modes (XAOAI_PCMTEE):
 *   1 = ch<XAOAI_PCMTEE_CH, default 0> 提取 + 48k->16k 三点均值抽取 -> FIFO /tmp/xiaoai-agent/pcm16.fifo
 *       (O_NONBLOCK: 无读者则丢弃该帧包, 引擎永不阻塞)
 *   2 = raw 8ch 直通 FIFO /tmp/xiaoai-agent/pcm8ch.fifo
 *   3 = 诊断: 收集 3s 原始 8ch 到 /tmp/xiaoai-agent/chdiag.raw 后自动回退 mode1
 * dgram taps/ 监控通道保留 (次要)。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#define TAPDIR "/tmp/xiaoai-agent/taps"
#define FIFO1 "/tmp/xiaoai-agent/pcm16.fifo"
#define FIFO2 "/tmp/xiaoai-agent/pcm8ch.fifo"
#define CHDIAG "/tmp/xiaoai-agent/chdiag.raw"
#define CAP_CH 8
#define CAP_HZ 48000
#define OUT_HZ 16000
#define DEC (CAP_HZ / OUT_HZ)
#define CHDIAG_BYTES (5 * CAP_HZ * CAP_CH * 2)
#define MAXT 16
typedef long sframes_t;
typedef short s16_t;

static sframes_t (*real_readi)(void *, void *, unsigned long) = NULL;
static int g_init = 0, g_on = 0, g_ch = 0;
static int g_fifo1 = -1, g_fifo2 = -1, g_diagfd = -1;
static unsigned long g_diag_bytes = 0;
static int tsock[MAXT];
static struct sockaddr_un taddr[MAXT];
static socklen_t tlen[MAXT];
static int ntaps = 0, dirty = 1;
static unsigned long g_calls = 0, g_frames = 0, g_sentb = 0, g_skip = 0, g_errs = 0;
static int g_lasterr = 0;
static time_t g_t0 = 0;

static void rescan(void)
{
    int i;
    for (i = 0; i < ntaps; i++) close(tsock[i]);
    ntaps = 0;
    DIR *d = opendir(TAPDIR);
    if (!d) { dirty = 0; return; }
    struct dirent *e;
    while ((e = readdir(d)) && ntaps < MAXT) {
        if (e->d_name[0] == '.') continue;
        int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) continue;
        memset(&taddr[ntaps], 0, sizeof(taddr[ntaps]));
        taddr[ntaps].sun_family = AF_UNIX;
        snprintf(taddr[ntaps].sun_path, sizeof(taddr[ntaps].sun_path), "%s/%s", TAPDIR, e->d_name);
        tlen[ntaps] = (socklen_t)(strlen(taddr[ntaps].sun_path) + offsetof(struct sockaddr_un, sun_path) + 1);
        tsock[ntaps++] = fd;
    }
    closedir(d);
    dirty = 0;
}

static void fanout_dgram(const void *buf, unsigned long n)
{
    int i;
    if (dirty || ((g_calls & 0x1FF) == 0)) rescan();
    for (i = 0; i < ntaps; i++) {
        ssize_t r = sendto(tsock[i], buf, n, MSG_NOSIGNAL, &taddr[i], tlen[i]);
        if (r < 0) { g_errs++; g_lasterr = errno; if (errno == ENOENT) dirty = 1; }
    }
}

static void stat_line(void)
{
    time_t now = time(NULL);
    if (now - g_t0 >= 5) {
        FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
        if (f) {
            fprintf(f, "stat calls=%lu frames=%lu sentb=%lu skip=%lu errs=%lu(errno=%d) fifo=%d ntaps=%d diag=%lu\n",
                    g_calls, g_frames, g_sentb, g_skip, g_errs, g_lasterr, g_fifo1 >= 0 ? 1 : 0, ntaps, g_diag_bytes);
            fclose(f);
        }
        g_t0 = now;
    }
}

static int open_fifo_nb(const char *path)
{
    mkfifo(path, 0666);
    return open(path, O_WRONLY | O_NONBLOCK); /* -1+ENXIO = 无读者 */
}

/* 重试退避: 首次立即尝试; 之后每 512 次 readi 才重试一次, 避免高频 mkfifo/open 打系统 */
static int fifo_retry_ok(void)
{
    static int first = 1;
    if (first) { first = 0; return 1; }
    return (g_calls & 0x1FF) == 0;
}

static void ptee_init(void)
{
    g_init = 1;
    real_readi = (sframes_t (*)(void *, void *, unsigned long))dlsym(RTLD_NEXT, "snd_pcm_readi");
    const char *g = getenv("XAOAI_PCMTEE");
    const char *c = getenv("XAOAI_PCMTEE_CH");
    if (c) g_ch = atoi(c);
    if (g && g[0] >= '1' && g[0] <= '3' && real_readi) g_on = g[0] - '0';
    if (g_on == 3) g_diagfd = open(CHDIAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fprintf(stderr, "pcmtee: init on=%d ch=%d\n", g_on, g_ch);
    FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
    if (f) { fprintf(f, "init on=%d ch=%d\n", g_on, g_ch); fclose(f); }
    g_t0 = time(NULL);
}

static s16_t g_out[8192];
static int g_phase = 0;

sframes_t snd_pcm_readi(void *pcm, void *buffer, unsigned long frames)
{
    if (!g_init) ptee_init();
    sframes_t r = real_readi(pcm, buffer, frames);
    if (g_on && r > 0) {
        unsigned long bytes = (unsigned long)r * CAP_CH * 2UL;
        g_calls++;
        g_frames += (unsigned long)r;

        if (g_on == 3) { /* 诊断: 收满 3s 原始流后回退 mode1 */
            if (g_diagfd >= 0) {
                size_t left = (size_t)(CHDIAG_BYTES - g_diag_bytes);
                ssize_t w = write(g_diagfd, buffer, bytes < left ? bytes : left);
                if (w > 0) g_diag_bytes += (unsigned long)w;
                if (g_diag_bytes >= CHDIAG_BYTES) {
                    close(g_diagfd); g_diagfd = -1;
                    FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
                    if (f) { fprintf(f, "chdiag done %lu bytes\n", g_diag_bytes); fclose(f); }
                    g_on = 1;
                }
            }
            return r;
        }

        if (g_on == 2) {
            if (g_fifo2 < 0 && fifo_retry_ok()) g_fifo2 = open_fifo_nb(FIFO2);
            if (g_fifo2 >= 0) {
                ssize_t w = write(g_fifo2, buffer, bytes);
                if (w < 0 && errno == ENXIO) { close(g_fifo2); g_fifo2 = -1; g_skip++; }
                else if (w < 0) g_skip++;
                else g_sentb += (unsigned long)w;
            } else g_skip++;
            fanout_dgram(buffer, bytes);
        } else {
            const s16_t *in = (const s16_t *)buffer;
            unsigned long f, n = 0;
            int acc = 0, k = g_phase;
            for (f = 0; f < (unsigned long)r && n + 1 < sizeof(g_out) / sizeof(g_out[0]); f++) {
                acc += in[f * CAP_CH + g_ch];
                if (++k == DEC) {
                    g_out[n++] = (s16_t)(acc / DEC);
                    acc = 0; k = 0;
                }
            }
            g_phase = k;
            if (n > 0) {
                unsigned long nb = n * 2UL;
                if (g_fifo1 < 0 && fifo_retry_ok()) g_fifo1 = open_fifo_nb(FIFO1);
                if (g_fifo1 >= 0) {
                    ssize_t w = write(g_fifo1, g_out, nb);
                    if (w < 0 && errno == ENXIO) { close(g_fifo1); g_fifo1 = -1; g_skip++; }
                    else if (w < 0) g_skip++;
                    else g_sentb += (unsigned long)w;
                } else g_skip++;
                fanout_dgram(g_out, nb);
            }
        }
        stat_line();
    }
    return r;
}
