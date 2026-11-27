/* LX06 噪音抑制 tap 读取端 · armhf 静态
 *
 * 用法: xaudio-tap-read --name <kws|rec|xz|db|...> [-f s16|s32] [-d 秒]
 *
 * 绑定 /tmp/xiaoai-agent/taps/<name>.usock（AF_UNIX dgram），接收
 * xaudio-tap 代理 fan-out 的上行报文，解析 mipns 原生 protobuf，
 * 只把 UP_STREAM_TRANSMITTING 的音频帧（16 kHz mono）以原始 PCM 写到
 * stdout；register/prepare/cancel/tts-finish 等控制帧全部丢弃：
 *
 *   顶层:  field1 varint = SPEECH_TYPE (0=UPWARD，其余丢弃)
 *          field2 len    = body
 *   body:  field1 varint = up_type (3=UP_STREAM_TRANSMITTING，其余丢弃)
 *          field4 len    = data { field1 varint 类型(0=pcm16,1=opus→丢弃)
 *                                  field2 len   = pcm16 帧 }
 *
 * 输出格式（-f）:
 *   s16  原样 16 kHz mono s16le（默认；小智/豆包等自定义链路直接用）
 *   s32  s16<<8 的 s32le。wakeword_template 读入后 >>8 还原成 s16。
 *        不能用 <<16：>>8 后会把底噪钳成满幅，VAD 永远判不到语音。
 * 输出按 480 样本块（s16:960B / s32:1920B，30ms）写出，边界与
 * arecord --buffer-size 480 一致。
 *
 * -d N: N 秒后自动退出（录音硬上限，对齐 arecord -d 5）。
 * 无数据（agent/tap 未就绪）→ 阻塞等待，不报错（由调用方探测逻辑决定
 * 兜底）；管道对端关闭（EPIPE）→ 静默退出（head 截断探测场景）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include "glibc_ni_trap.h"

static const char *g_name = NULL;
static int g_s32 = 0;
static double g_deadline = -1; /* 秒 */

/* ---------- mini protobuf 读取（varint + length-delimited，宽容解析） ---------- */

static int rd_varint(const uint8_t **p, const uint8_t *end, unsigned long long *out)
{
    unsigned long long v = 0;
    int sh = 0;
    while (*p < end) {
        uint8_t b = **p;
        (*p)++;
        v |= (unsigned long long)(b & 0x7f) << sh;
        if (!(b & 0x80)) { *out = v; return 1; }
        sh += 7;
        if (sh > 63) return 0;
    }
    return 0;
}

/* 返回 pcm16 帧指针与长度；非 TRANSMITTING/opus/解析失败 → NULL */
static const uint8_t *parse_audio(const uint8_t *p, const uint8_t *end, size_t *outlen)
{
    const uint8_t *bp = NULL;
    size_t blen = 0;
    while (p < end) {
        unsigned long long tag;
        if (!rd_varint(&p, end, &tag)) return NULL;
        unsigned long long field = tag >> 3, wire = tag & 7;
        if (wire == 0) {
            unsigned long long x;
            if (!rd_varint(&p, end, &x)) return NULL;
            if (field == 1 && x != 0) return NULL; /* 只认 SPEECH_TYPE_UPWARD */
        } else if (wire == 2) {
            unsigned long long l;
            if (!rd_varint(&p, end, &l)) return NULL;
            if ((size_t)(end - p) < l) return NULL;
            if (field == 2) { bp = p; blen = (size_t)l; } /* 其余 len 字段跳过 */
            p += l;
        } else {
            return NULL; /* wire 1/5 不支持 */
        }
    }
    if (!bp) return NULL;

    int have_transmit = 0;
    p = bp;
    const uint8_t *bend = bp + blen;
    while (p < bend) {
        unsigned long long tag;
        if (!rd_varint(&p, bend, &tag)) return NULL;
        unsigned long long field = tag >> 3, wire = tag & 7;
        if (wire == 0) {
            unsigned long long x;
            if (!rd_varint(&p, bend, &x)) return NULL;
            if (field == 1) have_transmit = (x == 3); /* UP_STREAM_TRANSMITTING */
        } else if (wire == 2) {
            unsigned long long l;
            if (!rd_varint(&p, bend, &l)) return NULL;
            if ((size_t)(bend - p) < l) return NULL;
            if (field == 4) {
                /* data: field1 varint(0=pcm16,1=opus) / field2 len = pcm16 */
                const uint8_t *dp = p, *dend = p + l;
                const uint8_t *pcm = NULL;
                size_t pl = 0;
                while (dp < dend) {
                    unsigned long long t2;
                    if (!rd_varint(&dp, dend, &t2)) { pcm = NULL; break; }
                    unsigned long long f2 = t2 >> 3, w2 = t2 & 7;
                    if (w2 == 0) {
                        unsigned long long x;
                        if (!rd_varint(&dp, dend, &x)) { pcm = NULL; break; }
                        if (f2 == 1 && x != 0) { pcm = NULL; break; } /* opus → 丢弃 */
                    } else if (w2 == 2) {
                        unsigned long long x;
                        if (!rd_varint(&dp, dend, &x)) { pcm = NULL; break; }
                        if ((size_t)(dend - dp) < x) { pcm = NULL; break; }
                        if (f2 == 2) { pcm = dp; pl = (size_t)x; }
                        dp += x;
                    } else { pcm = NULL; break; }
                }
                if (pcm && have_transmit) { *outlen = pl; return pcm; }
            }
            p += l;
        } else {
            return NULL;
        }
    }
    return NULL;
}

/* ---------- 输出（480 样本块对齐，s16/s32） ---------- */

#define CHUNK_SAMPLES 480

static uint16_t g_s16[CHUNK_SAMPLES];
static int g_ns = 0;

static int out_ok = 1;

static void write_all(const void *buf, size_t n)
{
    const uint8_t *q = buf;
    while (n > 0) {
        ssize_t r = write(1, q, n);
        if (r < 0) { if (errno == EINTR) continue; out_ok = 0; return; }
        q += r;
        n -= (size_t)r;
    }
}

static void flush_full(void)
{
    while (out_ok && g_ns >= CHUNK_SAMPLES) {
        if (!g_s32) {
            write_all(g_s16, CHUNK_SAMPLES * 2);
            memmove(g_s16, g_s16 + CHUNK_SAMPLES,
                    (size_t)(g_ns - CHUNK_SAMPLES) * 2);
            g_ns -= CHUNK_SAMPLES;
        } else {
            int32_t c[CHUNK_SAMPLES];
            int i;
            for (i = 0; i < CHUNK_SAMPLES; i++)
                c[i] = (int32_t)(int16_t)g_s16[i] << 8;
            write_all(c, sizeof(c));
            memmove(g_s16, g_s16 + CHUNK_SAMPLES,
                    (size_t)(g_ns - CHUNK_SAMPLES) * 2);
            g_ns -= CHUNK_SAMPLES;
        }
    }
}

int main(int argc, char **argv)
{
    int i, fd;
    char path[224];
    struct timespec t_start = { 0, 0 };
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name")) {
            if (i + 1 >= argc) return 2;
            g_name = argv[++i];
        } else if (!strcmp(argv[i], "-f")) {
            if (i + 1 >= argc) return 2;
            g_s32 = !strcmp(argv[++i], "s32");
        } else if (!strcmp(argv[i], "-d")) {
            if (i + 1 >= argc) return 2;
            g_deadline = atof(argv[++i]);
        } else {
            fprintf(stderr, "xaudio-tap-read: unknown arg %s\n", argv[i]);
            return 2;
        }
    }
    if (!g_name) {
        fprintf(stderr, "usage: xaudio-tap-read --name <n> [-f s16|s32] [-d sec]\n");
        return 2;
    }
    snprintf(path, sizeof(path), "/tmp/xiaoai-agent/taps/%s.usock", g_name);
    /* 自包含：父目录不存在时自建（生产环境里 proxy 先建，EEXIST 无害） */
    if (mkdir("/tmp", 0755) < 0 && errno != EEXIST) { perror("mkdir /tmp"); return 1; }
    if (mkdir("/tmp/xiaoai-agent", 0755) < 0 && errno != EEXIST) { perror("mkdir agent"); return 1; }
    if (mkdir("/tmp/xiaoai-agent/taps", 0755) < 0 && errno != EEXIST) { perror("mkdir taps"); return 1; }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("bind tap socket");
        return 1;
    }
    if (g_deadline > 0)
        clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (;;) {
        int timeout_ms = -1;
        if (g_deadline > 0) {
            struct timespec t1;
            double remain;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            remain = g_deadline - (t1.tv_sec - t_start.tv_sec)
                               - (t1.tv_nsec - t_start.tv_nsec) / 1e9;
            if (remain <= 0) break;
            timeout_ms = (int)(remain * 1000);
            if (timeout_ms > 1000) timeout_ms = 1000;
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue; /* poll ≤1s 封顶超时：继续等到 -d 到点（顶部重算） */
        if (!out_ok) break;
        uint8_t buf[8192];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;
        size_t alen = 0;
        const uint8_t *pcm = parse_audio(buf, buf + n, &alen);
        if (!pcm || (alen & 1) || alen >= sizeof(buf)) continue;
        size_t samples = alen / 2;
        if ((size_t)g_ns + samples > CHUNK_SAMPLES) {
            /* 超大帧：先刷出已有，再分块追加（正常帧远小于 480 样本） */
            while ((size_t)g_ns + samples > CHUNK_SAMPLES) {
                if (g_ns >= CHUNK_SAMPLES) { flush_full(); continue; }
                size_t take = CHUNK_SAMPLES - (size_t)g_ns;
                memcpy(g_s16 + g_ns, pcm, take * 2);
                g_ns = CHUNK_SAMPLES;
                pcm += take * 2;
                samples -= take;
                flush_full();
            }
            if (samples > 0) {
                memcpy(g_s16 + g_ns, pcm, samples * 2);
                g_ns += (int)samples;
            }
        } else {
            memcpy(g_s16 + g_ns, pcm, samples * 2);
            g_ns += (int)samples;
        }
        flush_full();
    }
    /* 尾部不足一块的样本丢弃（录音按整秒边界，误差 <30ms） */
    close(fd);
    unlink(path);
    return 0;
}
