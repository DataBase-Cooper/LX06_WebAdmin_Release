/* pcmtee.c v17 — LD_PRELOAD tee on engine's ALSA capture (snd_pcm_readi)
 * 铁律: 不 include <time.h> / 不用 atoi (设备 glibc 无 __time64/__isoc23_*, 惰性绑定失败即死);
 *       时间用裸 syscall(__NR_gettimeofday), 数字手工解析。
 * 通道 = unix DGRAM /tmp/xiaoai-agent/taps/<name>.usock (v13 已验证); 每 64 次 readi 重扫 (发现≤0.5s)。
 * 模式 (XAOAI_PCMTEE):
 *   1 = ch<XAOAI_PCMTEE_CH 默认0> + 48k->16k 三点均值抽取 (16k mono s16)
 *   2 = raw 8ch 直通
 *   3 = 诊断: 5s 原始 8ch 存 /tmp/xiaoai-agent/chdiag.raw 后回退 mode1
 * 每 5s 写 /tmp/xiaoai-agent/pcmtee.trace 一行 stat。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#define TAPDIR "/tmp/xiaoai-agent/taps"
#define CHDIAG "/tmp/xiaoai-agent/chdiag.raw"
#define CAP_CH 8
#define CAP_HZ 48000
#define OUT_HZ 16000
#define DEC (CAP_HZ / OUT_HZ)
#define RESCAN_MASK 0x3FUL
#define CHDIAG_BYTES (5UL * CAP_HZ * CAP_CH * 2UL)
#define MAXT 16
typedef long sframes_t;
typedef short s16_t;

static unsigned long now_ms(void)
{
    struct { long sec; long usec; } tv;
    syscall(__NR_gettimeofday, &tv, 0);
    return (unsigned long)tv.sec * 1000UL + (unsigned long)tv.usec / 1000UL;
}

static int parse_int(const char *s)
{
    int v = 0;
    if (!s) return -1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static sframes_t (*real_readi)(void *, void *, unsigned long) = NULL;
static int g_init = 0, g_on = 0, g_ch = 0;
static int g_diagfd = -1;
static unsigned long g_diag_bytes = 0;
static int tsock[MAXT];
static struct sockaddr_un taddr[MAXT];
static socklen_t tlen[MAXT];
static int ntaps = 0, dirty = 1;
static unsigned long g_calls = 0, g_frames = 0, g_sentb = 0, g_errs = 0, g_sentpkts = 0;
static int g_lasterr = 0;
static unsigned long g_t0 = 0;

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

static void fanout(const void *buf, unsigned long n)
{
    int i;
    if (dirty || ((g_calls & RESCAN_MASK) == 0)) rescan();
    for (i = 0; i < ntaps; i++) {
        ssize_t r = sendto(tsock[i], buf, n, MSG_NOSIGNAL, (const struct sockaddr *)&taddr[i], tlen[i]);
        if (r < 0) { g_errs++; g_lasterr = errno; if (errno == ENOENT) dirty = 1; }
        else { g_sentpkts++; g_sentb += (unsigned long)r; }
    }
}

static void stat_line(void)
{
    unsigned long now = now_ms();
    if (now - g_t0 >= 5000UL) {
        FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
        if (f) {
            fprintf(f, "stat calls=%lu frames=%lu sentpkts=%lu sentb=%lu errs=%lu(errno=%d) ntaps=%d diag=%lu\n",
                    g_calls, g_frames, g_sentpkts, g_sentb, g_errs, g_lasterr, ntaps, g_diag_bytes);
            fclose(f);
        }
        g_t0 = now;
    }
}

static void ptee_init(void)
{
    g_init = 1;
    real_readi = (sframes_t (*)(void *, void *, unsigned long))dlsym(RTLD_NEXT, "snd_pcm_readi");
    const char *g = getenv("XAOAI_PCMTEE");
    const char *c = getenv("XAOAI_PCMTEE_CH");
    if (c) { int v = parse_int(c); if (v >= 0 && v < CAP_CH) g_ch = v; }
    if (g && g[0] >= '1' && g[0] <= '3' && real_readi) g_on = g[0] - '0';
    if (g_on == 3) g_diagfd = open(CHDIAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fprintf(stderr, "pcmtee: init on=%d ch=%d\n", g_on, g_ch);
    FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
    if (f) { fprintf(f, "init on=%d ch=%d\n", g_on, g_ch); fclose(f); }
    g_t0 = now_ms();
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

        if (g_on == 3) { /* 诊断: 收满 5s 原始流后回退 mode1 */
            if (g_diagfd >= 0) {
                unsigned long left = CHDIAG_BYTES - g_diag_bytes;
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
            fanout(buffer, bytes);
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
            if (n > 0) fanout(g_out, n * 2UL);
        }
        stat_line();
    }
    return r;
}
