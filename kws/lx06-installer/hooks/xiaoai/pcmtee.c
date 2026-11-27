/* pcmtee.c — LD_PRELOAD tee on engine's ALSA capture (snd_pcm_readi)
 * V44 data plane: engine sole-opens /dev/snd/pcmC0D3c (S16_LE 8ch@48k), reads via libasound readi.
  * Gate: XAOAI_PCMTEE=1 (ch0 + 48k->16k decimated mono fanout) / =2 (raw 8ch full tee).
  * Fanout to /tmp/xiaoai-agent/taps/*.usock (SOCK_DGRAM). Return value untouched. */
 #define _GNU_SOURCE
 #include <dlfcn.h>
 __asm__(".symver dlsym,dlsym@GLIBC_2.4");
 #include <sys/socket.h>
 #include <sys/un.h>
 #include <dirent.h>
 #include <string.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <errno.h>

 #define TAPDIR "/tmp/xiaoai-agent/taps"
 #define CAP_CH 8
 #define CAP_HZ 48000
 #define OUT_HZ 16000
 #define DEC (CAP_HZ / OUT_HZ)
 #define MAXT 16
 typedef long sframes_t;
 typedef short s16_t;

static sframes_t (*real_readi)(void *, void *, unsigned long) = NULL;
static int g_init = 0, g_on = 0;
static int tsock[MAXT];
static struct sockaddr_un taddr[MAXT];
static socklen_t tlen[MAXT];
static int ntaps = 0, dirty = 1;
static unsigned long g_calls = 0, g_bytes = 0, g_sent = 0, g_errs = 0, g_frames = 0, g_sentb = 0;
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

static void fanout(const void *buf, unsigned long n)
{
    int i;
    if (dirty || ((g_calls & 0x1FF) == 0)) rescan();
    for (i = 0; i < ntaps; i++) {
        ssize_t r = sendto(tsock[i], buf, n, MSG_NOSIGNAL, &taddr[i], tlen[i]);
        if (r >= 0) g_sent++;
        else { g_errs++; g_lasterr = errno; if (errno == ENOENT) dirty = 1; }
    }
}

static void ptee_init(void)
{
    g_init = 1;
    real_readi = (sframes_t (*)(void *, void *, unsigned long))dlsym(RTLD_NEXT, "snd_pcm_readi");
    const char *g = getenv("XAOAI_PCMTEE");
    if (g && (g[0] == '1' || g[0] == '2') && real_readi) g_on = g[0] - '0';
    fprintf(stderr, "pcmtee: init on=%d real=%p\n", g_on, (void *)real_readi);
    FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
    if (f) { fprintf(f, "init on=%d\n", g_on); fclose(f); }
    g_t0 = time(NULL);
}

static void stat_line(void)
{
    time_t now = time(NULL);
    if (now - g_t0 >= 5) {
        FILE *f = fopen("/tmp/xiaoai-agent/pcmtee.trace", "a");
        if (f) {
            fprintf(f, "stat calls=%lu frames=%lu sentpkts=%lu sentbytes=%lu errs=%lu(errno=%d) ntaps=%d elapsed=%ld\n",
                    g_calls, g_frames, g_sent, g_sentb, g_errs, g_lasterr, ntaps, (long)(now - g_t0));
            fclose(f);
        }
        g_t0 = now;
    }
}

/* 8ch@48k interleaved in -> mode1: ch0 提取+三点均值抽取到 16k mono; mode2: raw 直通 */
static s16_t g_out[8192];
static int g_phase = 0; /* 跨调用抽取相位 */

sframes_t snd_pcm_readi(void *pcm, void *buffer, unsigned long frames)
{
    if (!g_init) ptee_init();
    sframes_t r = real_readi(pcm, buffer, frames);
    if (g_on && r > 0) {
        g_calls++;
        g_frames += (unsigned long)r;
        g_bytes += (unsigned long)r * CAP_CH * 2UL;
        if (g_on == 2) {
            fanout(buffer, (unsigned long)r * CAP_CH * 2UL);
        } else {
            const s16_t *in = (const s16_t *)buffer;
            unsigned long f, n = 0;
            int acc = 0, k = g_phase;
            /* 逐帧取 ch0，再做 DEC:1 三点均值抽取 */
            for (f = 0; f < (unsigned long)r && n + 1 < sizeof(g_out) / sizeof(g_out[0]); f++) {
                acc += in[f * CAP_CH];
                if (++k == DEC) {
                    g_out[n++] = (s16_t)(acc / DEC);
                    acc = 0; k = 0;
                }
            }
            g_phase = k;
            if (n > 0) { fanout(g_out, n * 2UL); g_sentb += n * 2UL; }
            g_sent += n;
        }
        stat_line();
    }
    return r;
}
