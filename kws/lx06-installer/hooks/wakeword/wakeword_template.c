/* wakeword_template v2 — LX06 唤醒技能（模板 DTW，Embedding 模式）
 *
 * 8 月 doubao-lx06 版本的演进版：DSP/段检测/DTW 原样保留，新增：
 *   - detect 逐词阈值:  LABEL=PATH[:THRESH]（缺省用 --threshold）
 *   - train:            从已保存录音文件训练模型（web88 录入流用）
 *   - capture:          stdin S16LE 16k 单声道 → 第一段语音 → WAV（VAD 自动裁空白）
 *   - score:            单条录音对模型评分（阈值测试按钮用）
 *
 * 用法:
 *   wakeword_template enroll LABEL MODEL [COUNT]
 *   wakeword_template detect [--threshold N] LABEL=PATH[:THRESH] ...
 *   wakeword_template train LABEL OUT.lwt REC1.wav [REC2.wav ...]
 *   wakeword_template capture OUT.wav [MAXSEC]
 *   wakeword_template score REC.wav LABEL=MODEL.lwt
 *
 * 录音文件: RIFF/WAVE 16k 单声道 S16LE（capture 输出格式），
 *           也接受裸 S16LE 16k 单声道（无 RIFF 头时按裸流处理）。
 * 编译: arm-linux-gnueabihf-gcc -O2 -static -ffast-math -o wakeword_template \
 *       wakeword_template.c -lm
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 静态 glibc time64 系统调用遮蔽（mico 4.9.61 无 403 等，防 do_ni_syscall 刷屏+拖慢） */
#include "../xiaoai/glibc_ni_trap.h"
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include "wake_policy.h"
#include "wake_pinyin.c"
#include "wake_forensics.c"
#include "wake_cloud_verify.c"
#include "wake_sample.c"

#ifndef WW_MFCC
#define WW_MFCC 0
#endif
#ifndef WW_EMBED_INT8
#define WW_EMBED_INT8 0
#endif
#if !WW_MFCC
#include "lwake.h"
#endif

#define SAMPLE_RATE 16000
#define PI_F 3.14159265358979323846
#define BLOCK_SAMPLES 320
#define PRE_BLOCKS 12
#define MAX_UTT_SAMPLES (SAMPLE_RATE * 5 / 2) /* 2.5s：最多 6 字唤醒词（原 1.5s 太短） */
#define MIN_UTT_SAMPLES (SAMPLE_RATE / 2)
#define FRAME_LEN 400
#define FRAME_STEP 160
#define FFT_N 512
#define MEL_BINS 128
#if WW_MFCC == 3
#define CEP 32
#else
#define CEP 13
#endif
#if WW_MFCC
#define FEAT_DIM CEP
/* MFCC and Embedding models and thresholds are deliberately incompatible. */
#if WW_MFCC == 3
#define MODEL_VERSION 8
#define FEATURE_NAME "mfcc-lifter32"
#elif WW_MFCC == 2
#define MODEL_VERSION 5
#define FEATURE_NAME "mfcc-no-c0"
#else
#define MODEL_VERSION 4
#define FEATURE_NAME "mfcc"
#endif
#else
#define FEAT_DIM LWAKE_DIM
#if WW_EMBED_INT8
#define MODEL_VERSION 7
#define FEATURE_NAME "embedding-int8"
#else
#define MODEL_VERSION 6
#define FEATURE_NAME "embedding-fp32"
#endif
#endif
#define MAX_FRAMES 400
#define MAX_TEMPLATES 10
#define MAX_MODELS 6
#define FEATURE_CONTEXT (SAMPLE_RATE * 32 / 100)

typedef struct { double r, i; } Complex;
typedef struct { uint32_t frames; float *feat; } Template;
typedef struct {
    char label[48];
    char path[256];
    float threshold;
    int has_threshold;
    uint32_t count;
    Template items[MAX_TEMPLATES];
} Model;

typedef struct {
    float noise;
    int speech_run;
    int silence_run;
    int active;
    int cooldown_blocks;
    int init_count;
    float init_min;
    int16_t pre[PRE_BLOCKS][BLOCK_SAMPLES];
    int pre_pos;
    int pre_count;
    int16_t utterance[MAX_UTT_SAMPLES];
    int utt_samples;
} Segmenter;

static void die(const char *msg) {
    fprintf(stderr, "wakeword: %s: %s\n", msg, strerror(errno));
    exit(2);
}

#if WW_MFCC
static float mel_filters[MEL_BINS][FFT_N / 2 + 1];
static float hann[FRAME_LEN], dct[CEP][MEL_BINS];
static int mel_first[MEL_BINS], mel_last[MEL_BINS];
/* local-wake extract_mfcc_features uses librosa defaults: Slaney mel scale,
 * 128 area-normalized bands, periodic Hann, centered zero padding, power dB
 * with an 80 dB floor, orthonormal DCT-II coefficients 0..12. */
static float hz_to_mel(float hz) {
    return hz < 1000.0f ? hz / (200.0f / 3.0f)
        : 15.0f + logf(hz / 1000.0f) / (logf(6.4f) / 27.0f);
}
static float mel_to_hz(float mel) {
    return mel < 15.0f ? mel * (200.0f / 3.0f)
        : 1000.0f * expf((mel - 15.0f) * (logf(6.4f) / 27.0f));
}

static void init_features(void) {
    for (int i = 0; i < FRAME_LEN; ++i)
        hann[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * i / FRAME_LEN);
    float bins[MEL_BINS + 2];
    float lo = hz_to_mel(0.0f), hi = hz_to_mel(SAMPLE_RATE / 2.0f);
    for (int i = 0; i < MEL_BINS + 2; ++i) {
        bins[i] = mel_to_hz(lo + (hi - lo) * i / (MEL_BINS + 1));
    }
    memset(mel_filters, 0, sizeof(mel_filters));
    for (int m = 0; m < MEL_BINS; ++m) {
        mel_first[m]=FFT_N/2; mel_last[m]=0;
        float a = bins[m], b = bins[m + 1], c = bins[m + 2];
        for (int k = 0; k <= FFT_N / 2; ++k) {
            float hz = (float)k * SAMPLE_RATE / FFT_N;
            float up = (hz - a) / (b - a), down = (c - hz) / (c - b);
            mel_filters[m][k] = fmaxf(0, fminf(up, down)) * 2.0f / (c - a);
            if(mel_filters[m][k]>0) {
                if(k<mel_first[m]) mel_first[m]=k;
                mel_last[m]=k;
            }
        }
    }
    for (int c = 0; c < CEP; ++c)
        for (int m = 0; m < MEL_BINS; ++m)
            dct[c][m] = cosf(PI_F * c * (m + 0.5f) / MEL_BINS)
                * sqrtf((c ? 2.0f : 1.0f) / MEL_BINS);
}

static void fft(Complex *x) {
    for (int i = 1, j = 0; i < FFT_N; ++i) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { Complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        double ang = -2.0 * PI_F / len;
        Complex wlen = {cos(ang), sin(ang)};
        for (int i = 0; i < FFT_N; i += len) {
            Complex w = {1.0f, 0.0f};
            for (int j = 0; j < len / 2; ++j) {
                Complex u = x[i + j];
                Complex v = {x[i + j + len / 2].r * w.r - x[i + j + len / 2].i * w.i,
                             x[i + j + len / 2].r * w.i + x[i + j + len / 2].i * w.r};
                x[i + j].r = u.r + v.r; x[i + j].i = u.i + v.i;
                x[i + j + len / 2].r = u.r - v.r; x[i + j + len / 2].i = u.i - v.i;
                double nr = w.r * wlen.r - w.i * wlen.i;
                w.i = w.r * wlen.i + w.i * wlen.r; w.r = nr;
            }
        }
    }
}

#if WW_MFCC == 3
/* Interior FFT/mel frames are identical across overlapping windows/words.
 * Store unclamped log-mel values; per-window peak clipping/DCT stays local.
 * Edge frames are never shared because their zero padding differs by window.
 * 400 slots cover the maximum 251-frame window with bounded ~203 KiB storage. */
static float mel_cache[MAX_FRAMES][MEL_BINS];
static uint64_t mel_cache_tags[MAX_FRAMES];
static int mel_cache_enabled;
static uint64_t mel_cache_origin;
#endif
static float *extract_features(const int16_t *pcm, int samples, uint32_t *out_frames) {
    if (samples < MIN_UTT_SAMPLES || samples > MAX_UTT_SAMPLES) return NULL;
    int frames = 1 + samples / FRAME_STEP;
    float *logs = malloc((size_t)frames * MEL_BINS * sizeof(float));
    float *feat = calloc((size_t)frames * CEP, sizeof(float));
    if (!logs || !feat) die("malloc mfcc");
    float peak = -100.0f;
    for (int t = 0; t < frames; ++t) {
#if WW_MFCC == 3
        uint64_t tag=mel_cache_origin/FRAME_STEP+(uint64_t)t+1;
        unsigned slot=(unsigned)(tag%MAX_FRAMES);
        int cached=mel_cache_enabled && t*FRAME_STEP>=FRAME_LEN/2 && t*FRAME_STEP+FRAME_LEN/2<=samples;
        if(cached && mel_cache_tags[slot]==tag) {
            for(int m=0;m<MEL_BINS;m++) {
                float db=mel_cache[slot][m];logs[t*MEL_BINS+m]=db;if(db>peak)peak=db;
            }
            continue;
        }
#endif
        Complex x[FFT_N] = {{0}};
        for (int i = 0; i < FRAME_LEN; ++i) {
            int at = t * FRAME_STEP - FRAME_LEN / 2 + i;
            if (at >= 0 && at < samples)
                x[i + (FFT_N - FRAME_LEN) / 2].r = pcm[at] / 32768.0f * hann[i];
        }
        fft(x);
        float power[FFT_N / 2 + 1];
        for (int k = 0; k <= FFT_N / 2; ++k)
            power[k] = x[k].r * x[k].r + x[k].i * x[k].i;
        for (int m = 0; m < MEL_BINS; ++m) {
            float energy = 0;
            for (int k = mel_first[m]; k <= mel_last[m]; ++k) energy += power[k] * mel_filters[m][k];
            float db = 10.0f * log10f(fmaxf(1e-10f, energy));
            logs[t * MEL_BINS + m] = db;
            if (db > peak) peak = db;
        }
#if WW_MFCC == 3
        if(cached) {
            memcpy(mel_cache[slot],logs+t*MEL_BINS,sizeof(mel_cache[slot]));
            mel_cache_tags[slot]=tag;
        }
#endif
    }
    /* Clamp each mel bin once, not once per cepstrum. A contiguous dot product
     * also lets the compiler vectorize DCT on the LX06 NEON unit. */
    for (int t = 0; t < frames; ++t) {
        float *row=logs+t*MEL_BINS;
        for(int m=0;m<MEL_BINS;m++)row[m]=fmaxf(row[m],peak-80.0f);
        for (int c = 0; c < CEP; ++c) {
            float sum=0;
            for (int m = 0; m < MEL_BINS; ++m)sum+=dct[c][m]*row[m];
            feat[t*CEP+c]=sum;
        }
    }
#if WW_MFCC == 3
    /* Preserve finer phonetic detail instead of letting the DC/lower cepstra
     * dominate cosine distance. This feature has its own model/gate identity. */
    for (int t=0;t<frames;t++) for(int c=0;c<CEP;c++)
        feat[t*CEP+c] *= c ? 1.0f+11.0f*sinf(PI_F*c/44.0f) : 0.0f;
#endif
    free(logs);
    *out_frames = (uint32_t)frames;
    return feat;
}
#else
static void init_features(void) {}
static float *extract_features(const int16_t *pcm, int samples, uint32_t *out_frames) {
    int frames, i, need;
    float *pcm_f, *feat;
    /* A normal four-character word can last <1s. The CNN's valid convolutions
     * consume context at both edges. Add context in feature space only; never
     * alter the recorded WAV or require the user to repeat/draw out the word.
     * Apply exactly the same transform to enrollment, scoring and detection.
     */
    if (samples < MIN_UTT_SAMPLES || samples > MAX_UTT_SAMPLES) return NULL;
    need = samples + 2 * FEATURE_CONTEXT;
    if (need < SAMPLE_RATE * 3 / 2) need = SAMPLE_RATE * 3 / 2;
    if (need > LWAKE_MAX_S) need = LWAKE_MAX_S;
    frames = lwake_max_frames(need);
    if (frames < 2 || frames > MAX_FRAMES) return NULL;
    pcm_f = calloc((size_t)need, sizeof(float));
    feat = calloc((size_t)frames * FEAT_DIM, sizeof(float));
    if (!pcm_f || !feat) die("malloc embed");
    int offset = (need - samples) / 2;
    if (offset < 0) { free(pcm_f); free(feat); return NULL; }
    for (i = 0; i < samples; ++i) pcm_f[offset + i] = pcm[i] / 32768.0f;
    frames = lwake_process(pcm_f, need, feat);
    free(pcm_f);
    if (frames < 2) { free(feat); return NULL; }
    *out_frames = (uint32_t)frames;
    return feat;
}
#endif

static float frame_cost(const float *a, const float *b) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    int k;
    for (k = WW_MFCC == 2 ? 1 : 0; k < FEAT_DIM; ++k) {
        dot += a[k] * b[k];
        na += a[k] * a[k];
        nb += b[k] * b[k];
    }
    if (na < 1e-12f || nb < 1e-12f) return 1.0f;
    return 1.0f - dot / sqrtf(na * nb);
}

static float dtw_distance(const float *a, int na, const float *b, int nb) {
    float ratio = (float)na / (float)nb;
    if (ratio < 0.55f || ratio > 1.80f) return 99.0f;
#if WW_MFCC == 3
    /* Cosine norms depend on a frame, not a DTW cell. Computing each once
     * avoids two vector reductions and a sqrt for every cell in every model.
     * Model/feature bytes and the mathematical distance remain unchanged. */
    if(na>MAX_FRAMES || nb>MAX_FRAMES)return 99.0f;
    float anorm[MAX_FRAMES],bnorm[MAX_FRAMES];
    for(int side=0;side<2;side++) {
        const float *v=side?b:a;float *norm=side?bnorm:anorm;int n=side?nb:na;
        for(int i=0;i<n;i++) {
            float power=0;for(int k=0;k<FEAT_DIM;k++)power+=v[i*FEAT_DIM+k]*v[i*FEAT_DIM+k];
            norm[i]=power<1e-12f?0.0f:1.0f/sqrtf(power);
        }
    }
#endif
#if WW_MFCC == 3
    float rows[2][MAX_FRAMES+1];
    float *prev=rows[0], *cur=rows[1];
#else
    float *prev = malloc((size_t)(nb + 1) * sizeof(float));
    float *cur = malloc((size_t)(nb + 1) * sizeof(float));
    if (!prev || !cur) die("malloc dtw");
#endif
    for (int j = 0; j <= nb; ++j) prev[j] = INFINITY;
    prev[0] = 0.0f;
    for (int i = 1; i <= na; ++i) {
        int center = (int)lrintf((float)i * nb / na);
        int band = WW_MFCC == 3 ? nb / 12 + 1 : nb / 3 + 3;
        int lo = center - band; if (lo < 1) lo = 1;
        int hi = center + band; if (hi > nb) hi = nb;
#if WW_MFCC == 3
        /* The center is monotone and advances by at most two columns at
         * the accepted length ratios. Only these edge cells can be read
         * from outside the previous row's computed band. */
        cur[lo-1]=INFINITY;
        if(hi<nb)cur[hi+1]=INFINITY;
        if(hi+1<nb)cur[hi+2]=INFINITY;
#else
        for (int j = 0; j <= nb; ++j) cur[j] = INFINITY;
#endif
        for (int j = lo; j <= hi; ++j) {
            float best = prev[j];
            if (cur[j - 1] < best) best = cur[j - 1];
            if (prev[j - 1] < best) best = prev[j - 1];
#if WW_MFCC == 3
            const float *av=a+(i-1)*FEAT_DIM,*bv=b+(j-1)*FEAT_DIM;
            float dot=0;for(int k=0;k<FEAT_DIM;k++)dot+=av[k]*bv[k];
            float cost=(!anorm[i-1] || !bnorm[j-1])?1.0f:1.0f-dot*anorm[i-1]*bnorm[j-1];
            cur[j]=best+cost;
#else
            cur[j] = best + frame_cost(a + (i - 1) * FEAT_DIM, b + (j - 1) * FEAT_DIM);
#endif
        }
        float *tmp = prev; prev = cur; cur = tmp;
    }
    float d = prev[nb] / (na + nb) + 0.35f * fabsf(logf(ratio));
#if WW_MFCC != 3
    free(prev); free(cur);
#endif
    return d;
}

static int save_model(const char *path, const char *label, Template *items, int count) {
    char tmp[320]; snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    uint32_t magic = 0x3154574cU, version = MODEL_VERSION, n = (uint32_t)count, labellen = (uint32_t)strlen(label);
    if (fwrite(&magic, 4, 1, f) != 1 || fwrite(&version, 4, 1, f) != 1 ||
        fwrite(&n, 4, 1, f) != 1 || fwrite(&labellen, 4, 1, f) != 1 ||
        fwrite(label, 1, labellen, f) != labellen) goto fail;
    for (int i = 0; i < count; ++i) {
        uint32_t dim = FEAT_DIM;
        if (fwrite(&items[i].frames, 4, 1, f) != 1 || fwrite(&dim, 4, 1, f) != 1 ||
            fwrite(items[i].feat, sizeof(float), (size_t)items[i].frames * FEAT_DIM, f) != (size_t)items[i].frames * FEAT_DIM) goto fail;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
fail:
    fclose(f); unlink(tmp); return -1;
}

/* 解析 "LABEL=PATH" 或 "LABEL=PATH:THRESH"（v2：逐模型阈值）。 */
static int parse_model_arg(const char *arg, char *label, size_t llab,
                           char *path, size_t lpath, float *th, int *has_th) {
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg || strlen(eq + 1) >= lpath) return -1;
    size_t ll = (size_t)(eq - arg);
    if (ll >= llab) return -1;
    memcpy(label, arg, ll); label[ll] = 0;
    const char *p = eq + 1;
    const char *colon = strrchr(p, ':');
    char tmp[300];
    float t = -1.0f;
    *has_th = 0;
    if (colon && colon > p) {
        size_t plen = (size_t)(colon - p);
        if (plen >= lpath) return -1;
        memcpy(tmp, p, plen); tmp[plen] = 0;
        char *end;errno=0;t = strtof(colon + 1, &end);
        uint32_t bits;memcpy(&bits,&t,sizeof(bits));
        /* isfinite alone is compiled away under -ffast-math. */
        if (errno || end==colon+1 || *end || (bits&0x7f800000U)==0x7f800000U || !(t>0.0f&&t<=2.0f)) return -1;
        *has_th = 1;
    } else {
        tmp[0] = 0;
        strcpy(tmp, p);
    }
    if (strlen(tmp) >= lpath) return -1;
    strcpy(path, tmp);
    *th = t;
    return 0;
}

static int load_model(Model *m, const char *arg) {
    if (parse_model_arg(arg, m->label, sizeof(m->label), m->path, sizeof(m->path),
                        &m->threshold, &m->has_threshold) != 0) return -1;
    FILE *f = fopen(m->path, "rb"); if (!f) return -1;
    uint32_t magic, version, count, labellen;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&count, 4, 1, f) != 1 || fread(&labellen, 4, 1, f) != 1 ||
        magic != 0x3154574cU || version != MODEL_VERSION || count < 1 || count > MAX_TEMPLATES || labellen > 255) goto fail;
    if (fseek(f, (long)labellen, SEEK_CUR) != 0) goto fail;
    m->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t dim;
        if (fread(&m->items[i].frames, 4, 1, f) != 1 || fread(&dim, 4, 1, f) != 1 ||
            dim != FEAT_DIM || m->items[i].frames < 2 || m->items[i].frames > MAX_FRAMES) goto fail;
        size_t n = (size_t)m->items[i].frames * FEAT_DIM;
        m->items[i].feat = malloc(n * sizeof(float)); if (!m->items[i].feat) die("malloc model");
        if (fread(m->items[i].feat, sizeof(float), n, f) != n) goto fail;
    }
    fclose(f); return 0;
fail:
    fclose(f); return -1;
}

static int read_block(int16_t *out) {
    /* s16le 直读（与官方 agent/训练 wav 同规格，保留全动态范围）。
     * 旧 s32 路径 <<16 后这里 >>8 净 ×256 裁剪 ±32767：VPM 1kHz 伪音
     * (s16 峰 128) 与语音 (s16 rms 4677) 被压到同一量级 → VAD 失真。 */
    static unsigned char raw[BLOCK_SAMPLES * 2];
    size_t got = 0;
    while (got < sizeof(raw)) {
        ssize_t n = read(STDIN_FILENO, raw + got, sizeof(raw) - got);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    for (int i = 0; i < BLOCK_SAMPLES; ++i) {
        out[i] = (int16_t)(raw[i * 2] | (raw[i * 2 + 1] << 8));
    }
    return 1;
}

static float block_rms(const int16_t *x) {
    double sum = 0.0; for (int i = 0; i < BLOCK_SAMPLES; ++i) sum += (double)x[i] * x[i];
    return (float)sqrt(sum / BLOCK_SAMPLES);
}

/* Returns 1 when a complete speech segment is available. */
static const float segment_gate = WW_MFCC == 3 ? 180.0f : 300.0f;
static int segment(Segmenter *s, const int16_t *block) {
    float rms = block_rms(block);
    /* 固定阈值，对齐官方 open-xiaoai agent CaptureConfig.threshold=0.006：
     * 官方 0.006 × 32768 = 197 s16（归一化 RMS 与 s16 RMS 同尺度）。
     * 本设备的 VPM 流底噪比官方 ALSA 高：实测 8s 纯静默 live8.raw
     * （22:48，会话稳定后）block(320) rms 55-146、峰 460 —— 全部 < 197。
     * 取 300（≈0.009）：对最大底噪块 146 留 70% 余量（22:46 会话不稳期
     * 嗡音曾 >197 造成 5s 假 WAKE）；对语音余量 15×+（3.wav rms 4677-5950）。 */
    /* Enrollment sees the stock processed stream too. Keep weak speech above
     * the measured <146 RMS floor; matching remains stricter below 300 RMS.
     * No shared-microphone gain or stock VPM setting is changed. */
    const float gate = segment_gate;
    /* 底噪 EMA 仅作诊断（不参与判决，防 -Wunused 亦无害） */
    s->noise = s->noise * 0.992f + rms * 0.008f;
    if (s->cooldown_blocks > 0) --s->cooldown_blocks;
    int voiced = rms > gate;

    memcpy(s->pre[s->pre_pos], block, BLOCK_SAMPLES * sizeof(int16_t));
    s->pre_pos = (s->pre_pos + 1) % PRE_BLOCKS; if (s->pre_count < PRE_BLOCKS) ++s->pre_count;

    if (!s->active) {
        s->speech_run = voiced ? s->speech_run + 1 : 0;
        if (s->speech_run >= 3 && s->cooldown_blocks == 0) {
            s->active = 1; s->silence_run = 0; s->utt_samples = 0;
            int start = (s->pre_pos - s->pre_count + PRE_BLOCKS) % PRE_BLOCKS;
            for (int i = 0; i < s->pre_count; ++i) {
                memcpy(s->utterance + s->utt_samples, s->pre[(start + i) % PRE_BLOCKS], BLOCK_SAMPLES * sizeof(int16_t));
                s->utt_samples += BLOCK_SAMPLES;
            }
        }
        return 0;
    }
    if (s->utt_samples + BLOCK_SAMPLES <= MAX_UTT_SAMPLES) {
        memcpy(s->utterance + s->utt_samples, block, BLOCK_SAMPLES * sizeof(int16_t));
        s->utt_samples += BLOCK_SAMPLES;
    }
    s->silence_run = voiced ? 0 : s->silence_run + 1;
    /* A natural four-character phrase ends after 500ms of silence. Long padded
     * TTS test prompts must not impose a one-second delay on every user wake. */
    if (s->silence_run >= 25 || s->utt_samples >= MAX_UTT_SAMPLES) {
        s->active = 0; s->speech_run = 0; s->pre_count = 0;
        while (s->utt_samples > BLOCK_SAMPLES && block_rms(s->utterance + s->utt_samples - BLOCK_SAMPLES) < gate)
            s->utt_samples -= BLOCK_SAMPLES;
        int voiced_blocks = 0;
        for (int i = 0; i + BLOCK_SAMPLES <= s->utt_samples; i += BLOCK_SAMPLES)
            if (block_rms(s->utterance + i) > gate) ++voiced_blocks;
        /* Sparse room-noise peaks can span >500ms without being an utterance.
         * Require 200ms of actual voiced frames, excluding the retained lead. */
        return s->utt_samples >= MIN_UTT_SAMPLES && voiced_blocks >= 10;
    }
    return 0;
}

/* ---- WAV（16k 单声道 S16LE）读写 ---- */

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v & 255); p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 255); p[1] = (unsigned char)((v >> 8) & 255);
    p[2] = (unsigned char)((v >> 16) & 255); p[3] = (unsigned char)((v >> 24) & 255);
}

static int write_wav(const char *path, const int16_t *pcm, int samples) {
    char tmp[320]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    uint32_t data = (uint32_t)(samples * 2);
    unsigned char hdr[44];
    memcpy(hdr, "RIFF", 4); put32(hdr + 4, 36u + data); memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4); put32(hdr + 16, 16);
    put16(hdr + 20, 1);                       /* PCM */
    put16(hdr + 22, 1);                       /* 单声道 */
    put32(hdr + 24, SAMPLE_RATE);
    put32(hdr + 28, (uint32_t)SAMPLE_RATE * 2);
    put16(hdr + 32, 2);                       /* block align */
    put16(hdr + 34, 16);                      /* bits */
    memcpy(hdr + 36, "data", 4); put32(hdr + 40, data);
    if (fwrite(hdr, 1, 44, f) != 44 ||
        fwrite(pcm, 2, (size_t)samples, f) != (size_t)samples) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* 0=ok。RIFF 头解析失败且文件 ≥1KB 时按裸 S16LE 处理（兼容手工录音）。 */
static int read_wav_bounded(const char *path, int16_t **out, int *out_samples, int truncate) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    long sz = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    if (sz < 1024 || (sz & 1)) { fclose(f); return -1; }
    unsigned char hd[16] = {0};
    int riff = (fread(hd, 1, 16, f) == 16) && !memcmp(hd, "RIFF", 4) && !memcmp(hd + 8, "WAVE", 4);
    long offset = 0, datsz = 0;
    if (riff) {
        fseek(f, 12, SEEK_SET);
        for (;;) {
            unsigned char ch[8];
            if (fread(ch, 1, 8, f) != 8) { fclose(f); return -1; }
            uint32_t id, csize;
            memcpy(&id, ch, 4); memcpy(&csize, ch + 4, 4);
            if (id == 0x61746164) { datsz = csize; offset = ftell(f); break; }  /* "data" LE */
            long skip = (long)(csize & ~1u) + (csize & 1);
            if (fseek(f, skip, SEEK_CUR) != 0) { fclose(f); return -1; }
        }
        if (datsz < 64) { fclose(f); return -1; }
    } else {
        offset = 0; datsz = (uint32_t)sz;
    }
    int samples = (int)(datsz / 2);
    if (samples > MAX_UTT_SAMPLES * 2) {
        if (!truncate) { fclose(f); fprintf(stderr,"matrix clip exceeds 5s; use detect stream for long background\n"); return -1; }
        samples = MAX_UTT_SAMPLES * 2;
    }
    int16_t *pcm = malloc((size_t)samples * 2);
    if (!pcm) { fclose(f); return -1; }
    fseek(f, offset, SEEK_SET);
    if (fread(pcm, 2, (size_t)samples, f) != (size_t)samples) { free(pcm); fclose(f); return -1; }
    fclose(f);
    *out = pcm; *out_samples = samples;
    return 0;
}

static int read_wav(const char *path, int16_t **out, int *out_samples) {
    return read_wav_bounded(path,out,out_samples,1);
}

/* Enrollment-only checks; do not change live detection/quiet-input gates. */
static int enrollment_quality(const char *path) {
    int16_t *pcm = NULL; int samples = 0, clipped = 0;
    double power = 0;
    if (read_wav_bounded(path, &pcm, &samples, 0)) {
        fprintf(stderr, "录音格式或完整性错误，请重录\n"); return 2;
    }
    for (int i = 0; i < samples; i++) {
        power += (double)pcm[i] * pcm[i];
        if (pcm[i] >= 32760 || pcm[i] <= -32760) clipped++;
    }
    free(pcm);
    double rms = samples ? sqrt(power / samples) : 0;
    printf("QUALITY duration_ms=%d rms=%.2f clipped=%d samples=%d\n",
           samples * 1000 / SAMPLE_RATE, rms, clipped, samples);
    if (samples < MIN_UTT_SAMPLES || samples > MAX_UTT_SAMPLES) {
        fprintf(stderr, "单条须为0.5–2.5秒，请自然说一遍并重录\n"); return 3;
    }
    /* Whole-utterance floor after trimming; capture retains its separate
     * 180 RMS block VAD. Clipping >1% is an enrollment quality failure,
     * never a reason to loosen a live wake gate. */
    if (rms < 90) { fprintf(stderr, "音量过低，请靠近或换安静环境重录\n"); return 3; }
    if (clipped * 100 > samples) { fprintf(stderr, "录音削波，请降低音量或稍远后重录\n"); return 3; }
    return 0;
}

static int enroll(const char *label, const char *path, int wanted) {
    Segmenter seg = {0}; Template items[MAX_TEMPLATES] = {{0}}; int count = 0;
    fprintf(stderr, "READY %s %d\n", label, wanted); fflush(stderr);
    int16_t block[BLOCK_SAMPLES];
    while (count < wanted) {
        int r = read_block(block); if (r <= 0) return 3;
        if (!segment(&seg, block)) continue;
        if (seg.utt_samples > SAMPLE_RATE * 5 / 2) {
            fprintf(stderr, "REJECTED duration=%.2fs (too long)\n", (double)seg.utt_samples / SAMPLE_RATE);
            fflush(stderr); seg.cooldown_blocks = 25; continue;
        }
        uint32_t frames = 0; float *feat = extract_features(seg.utterance, seg.utt_samples, &frames);
        /* 20→8 帧：CNN 下采样后 4 字词(~1.4s)只有 9 帧，20 帧只有 6 字词(2.5s=22帧)能达到；
         * 8 帧 = 0.78s 碎片(3帧)仍被拒，4/6 字词均通过。 */
        if (!feat || frames < 8) { free(feat); continue; }
        items[count].frames = frames; items[count].feat = feat; ++count;
        fprintf(stderr, "CAPTURED %d/%d %.2fs frames=%u\n", count, wanted, (double)seg.utt_samples / SAMPLE_RATE, frames); fflush(stderr);
        seg.cooldown_blocks = 35;
    }
    int rc = save_model(path, label, items, count);
    for (int i = 0; i < count; ++i) free(items[i].feat);
    if (rc != 0) die("save model");
    fprintf(stderr, "SAVED %s\n", path); return 0;
}

/* v2: 从文件训练。1..MAX_TEMPLATES 条录音；过短(<20帧)的跳过并计数。 */
static int train(const char *label, const char *path, int argc, char **argv) {
    Template items[MAX_TEMPLATES] = {{0}};
    int count = 0, skipped = 0;
    for (int i = 0; i < argc && count < MAX_TEMPLATES; ++i) {
        int16_t *pcm = NULL; int samples = 0;
        if (read_wav(argv[i], &pcm, &samples) != 0) {
            fprintf(stderr, "READFAIL %s\n", argv[i]); fflush(stderr);
            ++skipped; continue;
        }
        uint32_t frames = 0;
        float *feat = extract_features(pcm, samples, &frames);
        free(pcm);
        if (!feat || frames < 8) { /* 20→8 帧，与 capture 路径一致（4 字词 ~9 帧） */
            free(feat);
            fprintf(stderr, "TOOSHORT %s frames=%u\n", argv[i], frames); fflush(stderr);
            ++skipped; continue;
        }
        items[count].frames = frames; items[count].feat = feat; ++count;
        fprintf(stderr, "FRAME %s %.2fs frames=%u\n", argv[i], (double)samples / SAMPLE_RATE, frames);
        fflush(stderr);
    }
    if (count < 1) { fprintf(stderr, "NO_GOOD_RECORDINGS skipped=%d\n", skipped); return 2; }
    if (save_model(path, label, items, count) != 0) die("save model");
    for (int i = 0; i < count; ++i) free(items[i].feat);
    fprintf(stderr, "SAVED %s templates=%d skipped=%d\n", path, count, skipped);
    return 0;
}

/* Only full blocks which match preceding room noise may be removed. Keep
 * 60 ms before the first different block, including a soft consonant. */
static int raw_leading_silence(const Segmenter *seg, float background) {
    if (background < 0 || background > 60) return 0;
    float limit = fminf(60, 1.5f * background + 8);
    float peak_limit = fminf(180, 4 * limit);
    int quiet = 0;
    while (quiet < 9 && (quiet + 1) * BLOCK_SAMPLES <= seg->utt_samples) {
        const int16_t *p = seg->utterance + quiet * BLOCK_SAMPLES;
        if (block_rms(p) > limit) break;
        int i = 0;
        while (i < BLOCK_SAMPLES && abs((int)p[i]) <= peak_limit) ++i;
        if (i != BLOCK_SAMPLES) break;
        ++quiet;
    }
    int cut = quiet > 3 ? (quiet - 3) * BLOCK_SAMPLES : 0;
    return seg->utt_samples - cut >= MIN_UTT_SAMPLES ? cut : 0;
}

static int capture(const char *path, int maxsec, int (*next_block)(int16_t *), int raw_enrollment) {
    Segmenter seg = {0};
    int max_blocks = maxsec * SAMPLE_RATE / BLOCK_SAMPLES;
    int blocks = 0;
    int wait_quiet = 0;
    float background[8] = {0}, floor = -1;
    unsigned history = 0;
    int16_t block[BLOCK_SAMPLES];
    for (;;) {
        if (blocks >= max_blocks) { fprintf(stderr, "NO_UTT within %ds\n", maxsec); return 3; }
        int r = next_block(block);
        if (r <= 0) {
            fprintf(stderr, "NO_UTT (%s)\n", r < 0 ? "invalid audio stream" : "stream end");
            return 3;
        }
        ++blocks;
        if (r == 2) {
            if (wait_quiet || seg.active || seg.speech_run || block_rms(block) > segment_gate) wait_quiet = 25;
            memset(&seg, 0, sizeof(seg));
            history = 0;
            floor = -1;
        }
        if (wait_quiet) {
            if (block_rms(block) > segment_gate) wait_quiet = 25;
            else --wait_quiet;
            continue;
        }
        int was_active = seg.active;
        if (raw_enrollment && !was_active && seg.pre_count == PRE_BLOCKS)
            background[history++ % 8] = block_rms(seg.pre[seg.pre_pos]);
        if (segment(&seg, block)) break;
        if (raw_enrollment && !was_active && seg.active) {
            floor = -1;
            if (history >= 8) {
                float sum = 0;
                for (int i = 0; i < 8; ++i) sum += background[i];
                floor = sum / 8;
            }
        }
    }
    if (seg.utt_samples > SAMPLE_RATE * 5 / 2) {
        fprintf(stderr, "TOOLONG duration=%.2fs\n", (double)seg.utt_samples / SAMPLE_RATE);
        return 3;
    }
    int cut = raw_enrollment ? raw_leading_silence(&seg, floor) : 0;
    int samples = seg.utt_samples - cut;
    if (write_wav(path, seg.utterance + cut, samples) != 0) die("write wav");
    fprintf(stderr, "CAPTURED %.2fs samples=%d\n", (double)samples / SAMPLE_RATE, samples);
    return 0;
}

static float score_model(const Model *m, const float *feat, int frames) {
    float scores[MAX_TEMPLATES];
    for (uint32_t i = 0; i < m->count; ++i) scores[i] = dtw_distance(feat, frames, m->items[i].feat, m->items[i].frames);
    for (uint32_t i = 0; i < m->count; ++i) for (uint32_t j = i + 1; j < m->count; ++j)
        if (scores[j] < scores[i]) { float t = scores[i]; scores[i] = scores[j]; scores[j] = t; }
    return m->count > 1 ? 0.65f * scores[0] + 0.35f * scores[1] : scores[0];
}

/* v2: 单条文件对模型评分。stdout: SCORE <label> <score> [frames] */
static int score_file(const char *wav, const char *modelarg) {
    Model m = {0};
    if (load_model(&m, modelarg) != 0) { fprintf(stderr, "cannot load model: %s\n", modelarg); return 2; }
    int16_t *pcm = NULL; int samples = 0;
    if (read_wav(wav, &pcm, &samples) != 0) { fprintf(stderr, "cannot read wav: %s\n", wav); return 2; }
    uint32_t frames = 0;
    float *feat = extract_features(pcm, samples, &frames);
    free(pcm);
    /* Enrollment accepts 8-frame templates; score the same valid captures. */
    if (!feat || frames < 8) {
        free(feat);
        fprintf(stderr, "utterance too short frames=%u\n", frames);
        return 2;
    }
    float sc = score_model(&m, feat, (int)frames);
    printf("SCORE %s %.6f frames=%u\n", m.label, sc, frames);
    return 0;
}

#if WW_MFCC == 3
/* Feature/model bytes stay v8. Calibration identity also includes the live
 * window policy because a different search changes the score distribution. */
#define WW_DURATION_QUANTILES 4
#define WW_MAX_WINDOWS (WW_DURATION_QUANTILES + 1)
#define WW_QUIET_RMS 150.0f
#define WW_NORMAL_RMS 300.0f
#define WW_STRONG_RATIO .70f
#define WW_SCORE_HOP_BLOCKS 4
#define WW_DECISION_WAIT_BLOCKS 32

/* Preserve 240 ms for soft consonants and leave quiet utterances untouched. */
static int enrollment_tail_length(const int16_t *pcm, int samples) {
    int last_voice=0;
    float peak=0;
    for(int pos=0;pos+BLOCK_SAMPLES<=samples;pos+=BLOCK_SAMPLES) {
        float rms=block_rms(pcm+pos);
        if(rms>peak)peak=rms;
        if(rms>=WW_NORMAL_RMS)last_voice=pos+BLOCK_SAMPLES;
    }
    int end=last_voice+12*BLOCK_SAMPLES;
    if(peak<4*WW_NORMAL_RMS || end<MIN_UTT_SAMPLES ||
       samples-end<SAMPLE_RATE/2)return samples;
    return end;
}

/* Recognize both historical full-clip templates and the deterministic retry
 * used when a long room-noise tail prevents independent calibration. */
static int enrollment_template_samples(const int16_t *pcm, int samples,
                                       const Template *stored) {
    uint32_t frames=0;
    float *feat=extract_features(pcm,samples,&frames);
    int match=feat && frames==stored->frames &&
        !memcmp(feat,stored->feat,(size_t)frames*FEAT_DIM*sizeof *feat);
    free(feat);
    if(match)return samples;
    int end=enrollment_tail_length(pcm,samples);
    if(end==samples)return 0;
    feat=extract_features(pcm,end,&frames);
    match=feat && frames==stored->frames &&
        !memcmp(feat,stored->feat,(size_t)frames*FEAT_DIM*sizeof *feat);
    free(feat);
    return match ? end : 0;
}

static int model_windows(const Model *model, int *windows) {
    uint32_t lengths[MAX_TEMPLATES];
    for (uint32_t k=0;k<model->count;k++) lengths[k]=model->items[k].frames;
    for (uint32_t k=0;k<model->count;k++) for(uint32_t j=k+1;j<model->count;j++)
        if(lengths[j]<lengths[k]) {uint32_t n=lengths[k];lengths[k]=lengths[j];lengths[j]=n;}
    int count=0;
    for(int k=0;k<WW_DURATION_QUANTILES;k++) {
        unsigned index=(unsigned)k*(model->count-1)/(WW_DURATION_QUANTILES-1);
        int samples=(int)(lengths[index]-1)*FRAME_STEP;
        /* 80 ms buckets bound work and share nearby enrollment durations. */
        int hop=4*BLOCK_SAMPLES;
        samples=(samples+hop/2)/hop*hop;
        if(samples<MIN_UTT_SAMPLES) samples=MIN_UTT_SAMPLES;
        if(samples>MAX_UTT_SAMPLES) samples=MAX_UTT_SAMPLES;
        if(!count || samples!=windows[count-1]) windows[count++]=samples;
    }
    /* One intermediate duration bridges the first gap in enrolled cadence.
     * Templates still contain every frame from the original recordings. */
    const int hop=WW_SCORE_HOP_BLOCKS*BLOCK_SAMPLES;
    for(int i=1;i<count;i++) {
        int gap=windows[i]-windows[i-1];
        if(gap<2*hop)continue;
        int middle=windows[i-1]+(gap/(2*hop))*hop;
        memmove(windows+i+1,windows+i,(size_t)(count-i)*sizeof *windows);
        windows[i]=middle;
        ++count;
        break;
    }
    return count;
}
static float live_window_score(Model *model,const int16_t *samples,int window) {
    double power=0;
    for(int j=0;j<window;j++) power+=(double)samples[j]*samples[j];
    if(power/window<=WW_QUIET_RMS*WW_QUIET_RMS) return 99;
    uint32_t frames=0;float *feat=extract_features(samples,window,&frames);
    if(!feat) return 99;
    float score=score_model(model,feat,(int)frames);free(feat);
    if(power/window<=WW_NORMAL_RMS*WW_NORMAL_RMS && model->has_threshold &&
       score>model->threshold*WW_STRONG_RATIO)return 99;
    return score;
}
/* Score/gate/trails belong to each word, never to a shared winner. Only
 * independently eligible candidates enter arbitration. Wait up to 640 ms for
 * the rest of the pronunciation; a strong match (<=70% of its own gate) can
 * settle after 160 ms. This prevents an early weak partial match from consuming
 * the audio and cooling down the correct, slightly longer word. Ambiguity is
 * rejected after this observation interval, not before all words can finish.
 * No threshold is widened, and one duration still needs three confirmations. */
/* Normalized arbitration (§5.4.4 a/b). r = score/own gate. The winner is the
 * smallest r among words that pass their own gate. Other words with
 * r <= 1+B are competitors even when they missed their gate. Accept only when
 * there is no competitor or the normalized gap is at least M. */
static int wake_candidate_select(const float *scores,const float *gates,int count) {
    const float band=0.15f, need=0.10f;
    int best=-1; float first=99.f, second=99.f;
    for(int i=0;i<count;i++) {
        float gate=gates[i];
        if(!(gate>0.f)) continue;
        float ratio=scores[i]/gate;
        if(!(ratio<=1.f)) continue;
        if(ratio<first) { second=first; first=ratio; best=i; }
        else if(ratio<second) second=ratio;
    }
    if(best<0) return -1;
    for(int i=0;i<count;i++) {
        if(i==best) continue;
        float gate=gates[i];
        if(!(gate>0.f)) continue;
        float ratio=scores[i]/gate;
        if(ratio<=1.f+band && ratio<second) second=ratio;
    }
    if(!(second<90.f)) return best;
    return (second-first)>=need ? best : -1;
}
/* Kept only for offline A/B. Production detect does not call this. */
static int wake_candidate_select_raw030(const float *scores,const float *gates,int count) {
    int best=-1;float first=99,second=99;
    for(int i=0;i<count;i++) {
        if(!(scores[i]<=gates[i]))continue;
        if(scores[i]<first){second=first;first=scores[i];best=i;}
        else if(scores[i]<second)second=scores[i];
    }
    return best>=0 && second-first>=.030f ? best : -1;
}

/* An authenticated capture reader supplies this private pipe. One detector
 * process accepts exactly one source incarnation and idle epoch, so all of
 * its feature, confirmation and deferred-decision state has one owner.
 * The frame header cannot establish capture provenance by itself. */
static int wake_epoch_input;
#include "../xiaoai/wake-capture-frame.h"
_Static_assert(BLOCK_SAMPLES == LX06_WAKE_FRAME_SAMPLES, "wake epoch frame size");
static struct {
    unsigned char source[16], idle[16];
    uint64_t first, next, available_ms;
    struct {uint64_t end, available;} recent[WW_DECISION_WAIT_BLOCKS+2];
    int started;
} wake_epoch;

static int read_epoch_frame(int16_t *block, int capture_mode) {
    unsigned char header[LX06_WAKE_FRAME_HEADER];
    static const unsigned char zero[16];
    size_t got = 0;
    while (got < sizeof(header)) {
        ssize_t n = read(STDIN_FILENO, header + got, sizeof(header) - got);
        if (n == 0) return got ? -1 : 0;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)n;
    }
    if (memcmp(header, LX06_WAKE_FRAME_MAGIC, 4) || !memcmp(header + LX06_WAKE_FRAME_SOURCE, zero, 16) ||
        !memcmp(header + LX06_WAKE_FRAME_IDLE, zero, 16)) return -1;
    uint64_t first = 0, available_ms = 0;
    for (int i = 0; i < 8; ++i) {
        first |= (uint64_t)header[LX06_WAKE_FRAME_FIRST + i] << (8 * i);
        available_ms |= (uint64_t)header[LX06_WAKE_FRAME_AVAILABLE + i] << (8 * i);
    }
    if (!available_ms || first > UINT64_MAX - BLOCK_SAMPLES) return -1;
    int changed = wake_epoch.started && (memcmp(header + LX06_WAKE_FRAME_SOURCE, wake_epoch.source, 16) ||
        memcmp(header + LX06_WAKE_FRAME_IDLE, wake_epoch.idle, 16));
    if (wake_epoch.started && (available_ms < wake_epoch.available_ms || (!changed && first < wake_epoch.next)))
        return -1;
    int boundary = changed || (wake_epoch.started && first != wake_epoch.next);
    if (boundary && !capture_mode) return -1;
    if (read_block(block) != 1) return -1;
    if (!wake_epoch.started || boundary) {
        memcpy(wake_epoch.source, header + LX06_WAKE_FRAME_SOURCE, 16);
        memcpy(wake_epoch.idle, header + LX06_WAKE_FRAME_IDLE, 16);
        wake_epoch.first = first;
        wake_epoch.started = 1;
    }
    wake_epoch.next = first + BLOCK_SAMPLES;
    wake_epoch.available_ms = available_ms;
    size_t slot=((wake_epoch.next-wake_epoch.first)/BLOCK_SAMPLES)%
                (sizeof(wake_epoch.recent)/sizeof(wake_epoch.recent[0]));
    wake_epoch.recent[slot].end=wake_epoch.next;
    wake_epoch.recent[slot].available=available_ms;
    return boundary ? 2 : 1;
}
static int read_epoch_block(int16_t *block) {
    return read_epoch_frame(block, 0);
}
static int read_epoch_capture(int16_t *block) {
    return read_epoch_frame(block, 1);
}

static void epoch_hex(const unsigned char bytes[16], char output[33]) {
    static const char digit[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        output[2*i] = digit[bytes[i] >> 4];
        output[2*i + 1] = digit[bytes[i] & 15];
    }
    output[32] = '\0';
}

static void emit_wake(const char *label, float score, int samples, int scored_end_ms) {
    if (wake_epoch_input) {
        if(samples<=0 || samples%BLOCK_SAMPLES || scored_end_ms<=0 || scored_end_ms%20) return;
        uint64_t offset=(uint64_t)(scored_end_ms/20)*BLOCK_SAMPLES;
        if(offset<(unsigned)samples || offset>wake_epoch.next-wake_epoch.first) return;
        uint64_t end=wake_epoch.first+offset;
        size_t slot=(offset/BLOCK_SAMPLES)%(sizeof(wake_epoch.recent)/sizeof(wake_epoch.recent[0]));
        if(wake_epoch.recent[slot].end!=end) return;
        char source[33], idle[33];
        epoch_hex(wake_epoch.source, source);
        epoch_hex(wake_epoch.idle, idle);
        printf("WAKE_EPOCH %s %.6f %s %s %llu %llu %llu\n", label, score,
               source, idle, (unsigned long long)(end-(unsigned)samples),
               (unsigned long long)end, (unsigned long long)wake_epoch.recent[slot].available);
    } else {
        printf("WAKE %s %.6f\n", label, score);
    }
    fflush(stdout);
}
#include "wake-paired-input.inc"
#include "wake_runtime_p05.inc"
#include "wake-phrase-fast.inc"
static int wake_stable_single_candidate(int count, int now_ms, int max_window, const float *best,
        const float *gates, const int *ended, const int *width, const int *windows,
        const float trail[][WW_MAX_WINDOWS][3]) {
    int sole=-1;
    for(int i=0;i<count;i++) {
        if(!ended[i] || ended[i]==now_ms || gates[i]<=0.f) return 0;
        if(best[i]<=gates[i]*(1.f+WAKE_NORM_BAND)) {
            if(sole>=0 || best[i]>gates[i]) return 0;
            sole=i;
        }
        for(int w=0;w<windows[i];w++)
            for(int j=0;j<3;j++) if(trail[i][w][j]>=99.f) return 0;
    }
    return sole>=0 && width[sole]>=max_window;
}

/* A fixed, quiet tail can be evaluated before waiting for the entire
 * enrollment window. The ordinary window search continues independently.
 * Only the observed PCM is ever used for the physical pairing receipt. */
#define WW_EARLY_PAD_SAMPLES (8 * BLOCK_SAMPLES)
struct wake_early {
    float trail[MAX_MODELS][3];
    int hops;
};

static int wake_early_decide(struct wake_early *early, Model *models, int count,
                             const int16_t *pcm, int used, int width, int blocks,
                             const float *gates, int *cooldown, int *used_io) {
    int observed = width - WW_EARLY_PAD_SAMPLES;
    if (!paired || p05_raw_policy || *cooldown || observed < 24000 || used < observed ||
        count < 2 || p05_cloud_on()) {
        early->hops = 0;
        return 0;
    }
    const int16_t *source = pcm + used - observed;
    double peak = 0, tail = 0, energy = 0;
    for (int i=0; i<observed; i+=BLOCK_SAMPLES) {
        double power = 0;
        for (int j=0; j<BLOCK_SAMPLES; ++j)
            power += (double)source[i+j]*source[i+j];
        power /= BLOCK_SAMPLES;
        energy += power;
        if (i >= observed-8*BLOCK_SAMPLES) tail = fmax(tail,power);
        else peak = fmax(peak,power);
    }
    if (peak < 4.*WW_NORMAL_RMS*WW_NORMAL_RMS ||
        tail > fmax(WW_QUIET_RMS*WW_QUIET_RMS,.025*peak) ||
        energy*BLOCK_SAMPLES/observed <= WW_NORMAL_RMS*WW_NORMAL_RMS) {
        early->hops = 0;
        return 0;
    }
    /* Bound extra work per quiet period. Require three distinct observed
     * windows, each ending one real score hop after the previous one. */
    if (early->hops >= 8) return 0;
    int16_t projected[MAX_UTT_SAMPLES] = {0};
    memcpy(projected,source,(size_t)observed*sizeof(*source));
    uint32_t frames;
    int cache = mel_cache_enabled;
    mel_cache_enabled = 0;
    float *features = extract_features(projected,width,&frames);
    mel_cache_enabled = cache;
    if (!features) {
        early->hops = 0;
        return 0;
    }
    int slot = early->hops++ % 3;
    for (int i=0; i<count; ++i)
        early->trail[i][slot] = score_model(&models[i],features,(int)frames);
    free(features);
    if (early->hops < 3) return 0;

    float score[MAX_MODELS], rms[MAX_MODELS], snapshots[MAX_MODELS][3];
    int winner = -1, lengths[MAX_MODELS], ends[MAX_MODELS];
    for (int i=0; i<count; ++i) {
        lengths[i] = observed;
        ends[i] = blocks*20;
        rms[i] = (float)sqrt(energy*BLOCK_SAMPLES/observed);
        p05_save_trail(snapshots[i],early->trail[i],slot);
        score[i] = fmaxf(snapshots[i][0],fmaxf(snapshots[i][1],snapshots[i][2]));
        if (gates[i] <= 0.f) return 0;
        if (score[i] <= gates[i]) {
            if (winner >= 0) return 0;
            winner = i;
        }
    }
    if (winner < 0) return 0;
    for (int i=0; i<count; ++i) if (i != winner) {
        for (int j=0; j<3; ++j) {
            float rival = snapshots[i][j];
            if (rival/gates[i] <= 1.f+WAKE_NORM_BAND ||
                rival/gates[i] - score[winner]/gates[winner] < WAKE_GRAY_MARGIN ||
                rival-score[winner] < WAKE_LOCAL_STRONG_SEPARATION_MARGIN) return 0;
        }
    }
    if (gates[winner] > WAKE_AUTO_GATE_CEILING) return 0;
    struct pair_ticket ticket;
    if (!pair_prepare(&ticket,source,observed,ends[winner])) return 0;
    p05_decide_with_pair(models,count,score,rms,lengths,snapshots,ends,NULL,gates,
                         used,blocks,cooldown,used_io,&ticket);
    if (*cooldown) {
        fprintf(stderr,"EARLY_WAKE %s observed_ms=%d projected_tail_ms=%d\n",
                models[winner].label,observed/16,WW_EARLY_PAD_SAMPLES/16);
        early->hops = 0;
        return 1;
    }
    return 0;
}

static int detect_continuous(Model *models, int count, float fallback) {
    memset(mel_cache_tags,0,sizeof(mel_cache_tags));
    int window[MAX_MODELS][WW_MAX_WINDOWS], windows[MAX_MODELS], max_window=0;
    float trail[MAX_MODELS][WW_MAX_WINDOWS][3];
    for(int i=0;i<count;i++) {
        windows[i]=model_windows(&models[i],window[i]);
        for(int w=0;w<windows[i];w++) {
            if(window[i][w]>max_window)max_window=window[i][w];
            for(int j=0;j<3;j++)trail[i][w][j]=99;
            fprintf(stderr,"WINDOW_CONFIG %s samples=%d hop_ms=80 confirmations=3 policy=%s\n",models[i].label,window[i][w],WW_CALIBRATION_POLICY);
        }
    }
    int16_t pcm[MAX_UTT_SAMPLES], block[BLOCK_SAMPLES];
    enum { ENERGY_HISTORY=MAX_UTT_SAMPLES/BLOCK_SAMPLES+1 };
    uint64_t energy[ENERGY_HISTORY]={0};
    int used=0, blocks=0, cooldown=0, step=0, decision=0, first_pass=0, near_due=0;
    int wide_due=0,wide_mute_until=0;
    struct wake_early early = {0};
    struct wake_phrase phrase = {0};
    mel_cache_enabled=0;
    phrase_prepare(&phrase,models,count);
#ifndef WW_DISABLE_MEL_CACHE
    mel_cache_enabled=1;
#endif
    float pending[MAX_MODELS], best_seen[MAX_MODELS], best_rms[MAX_MODELS], win_rms[MAX_MODELS][WW_MAX_WINDOWS];
    int best_win[MAX_MODELS], weak_hit[MAX_MODELS], best_end_ms[MAX_MODELS] = {0};
    float best_trail[MAX_MODELS][3] = {{0}};
    for(int i=0;i<count;i++){pending[i]=best_seen[i]=99;best_rms[i]=0;best_win[i]=weak_hit[i]=0;}
    for(;;) {
        int r = paired ? read_paired_block(block) : wake_epoch_input ? read_epoch_block(block) : read_block(block);
        if(r<=0) {
            p05_cloud_reap();
            if (p05_ev.live) p05_flush_event();
            phrase_free(&phrase,count);
            return r<0 ? 3 : 0;
        }
        ++blocks; if(cooldown) --cooldown;
        uint64_t total=energy[(blocks-1)%ENERGY_HISTORY];
        for(int i=0;i<BLOCK_SAMPLES;++i)total+=(int32_t)block[i]*block[i];
        energy[blocks%ENERGY_HISTORY]=total;
        p05_ring_push(block);
        if (p05_ev.live && p05_post_left <= 0 && !p05_receipt_pending()) p05_flush_event();
        if(used+BLOCK_SAMPLES>max_window) {
            memmove(pcm,pcm+BLOCK_SAMPLES,(size_t)(used-BLOCK_SAMPLES)*sizeof(int16_t));
            used-=BLOCK_SAMPLES;
        }
        memcpy(pcm+used,block,sizeof(block));used+=BLOCK_SAMPLES;
        if (phrase_fast_decide(&phrase,models,count,pcm,used,blocks,fallback,&cooldown,&used)) {
            decision=first_pass=near_due=wide_due=0;
            early.hops=0;
            for (int i=0;i<count;++i) {
                pending[i]=best_seen[i]=99;
                weak_hit[i]=best_win[i]=best_end_ms[i]=0;
                best_rms[i]=0;
                memset(best_trail[i],0,sizeof(best_trail[i]));
                for (int w=0;w<windows[i];++w)
                    for (int j=0;j<3;++j) trail[i][w][j]=99;
            }
            continue;
        }
        if(blocks%WW_SCORE_HOP_BLOCKS) continue;
        /* Bounded step-local sharing: identical PCM start/length gets identical
         * features. Other lengths retain their own clipping/padding/DCT. */
        float scores[MAX_MODELS][WW_MAX_WINDOWS];
        for(int i=0;i<count;i++)for(int w=0;w<windows[i];w++)scores[i][w]=99;
        int lengths[MAX_MODELS*WW_MAX_WINDOWS],nlengths=0;
        for(int i=0;i<count;i++)for(int w=0;w<windows[i];w++) {
            int length=window[i][w],seen=0;
            for(int k=0;k<nlengths;k++)if(lengths[k]==length)seen=1;
            if(seen)continue;
            lengths[nlengths++]=length;
            if(used<length||cooldown)continue;
            const int16_t *samples=pcm+used-length;
            double power=(double)(total-energy[(blocks-length/BLOCK_SAMPLES)%ENERGY_HISTORY])/length;
            if(power<=WW_QUIET_RMS*WW_QUIET_RMS)continue;
            uint32_t frames=0;mel_cache_origin=(uint64_t)blocks*BLOCK_SAMPLES-length;
            float *feat=extract_features(samples,length,&frames);
            if(!feat)continue;
            float rms=(float)sqrt(power);
            for(int m=0;m<count;m++)for(int v=0;v<windows[m];v++)
                if(window[m][v]==length) {
                    float score=score_model(&models[m],feat,(int)frames);
                    float gate=models[m].has_threshold?models[m].threshold:fallback;
                    /* Quiet speech needs a stronger acoustic match, never a
                     * larger gate. Do not amplify the shared microphone or
                     * distort stock AEC/noise suppression for this detector. */
                    win_rms[m][v]=rms;
                    if(power>WW_NORMAL_RMS*WW_NORMAL_RMS || score<=gate*WW_STRONG_RATIO)
                        scores[m][v]=score;
                    else if(score<=gate*(1.f+WAKE_NORM_BAND)) weak_hit[m]=1;
                }
            free(feat);
        }
        for(int i=0;i<count;i++) {
            float confirmed=99; int chosen=0, chosen_index=-1; float chosen_rms=0;
            for(int w=0;w<windows[i];w++) {
                float score=scores[i][w];
                trail[i][w][step%3]=score;
                /* Confirm on one duration, never splice unrelated windows. */
                float stable=fmaxf(trail[i][w][0],fmaxf(trail[i][w][1],trail[i][w][2]));
                if(paired) p05_observe_score(step,blocks*20,i,w,window[i][w],stable,
                                             stable<99.f ? win_rms[i][w] : 0.f,trail[i][w]);
                if(stable<confirmed){confirmed=stable;chosen=window[i][w];chosen_index=w;chosen_rms=win_rms[i][w];}
            }
            float own_gate=models[i].has_threshold ? models[i].threshold : fallback;
            if(confirmed<99.f && confirmed<best_seen[i]) {
                best_seen[i]=confirmed;best_win[i]=chosen;best_rms[i]=chosen_rms;
                p05_save_trail(best_trail[i],trail[i][chosen_index],step);
                best_end_ms[i]=blocks*20;
            }
            if(confirmed<=own_gate) {
                if(!decision) {
                    /* The third confirming window has already passed. Keep
                     * two score hops for competition and physical alignment. */
                    int wait=(!p05_raw_policy && !p05_cloud_on()) ? 2*WW_SCORE_HOP_BLOCKS : WW_DECISION_WAIT_BLOCKS;
                    first_pass=blocks;
                    decision=blocks+wait;
                }
                if(confirmed<=own_gate*WW_STRONG_RATIO && decision>blocks+8)decision=blocks+8;
                if(confirmed<pending[i])pending[i]=confirmed;
            }
        }
        ++step;
        {
            int close=0,wide=0; float gates_now[MAX_MODELS];
            for(int i=0;i<count;i++){
                gates_now[i]=models[i].has_threshold?models[i].threshold:fallback;
                if(gates_now[i]<=0.f) continue;
                float ratio=best_seen[i]/gates_now[i];
                if(ratio<=1.f+WAKE_NORM_BAND) close=1;
                else if(best_end_ms[i]==blocks*20 && p05_wide_candidate(best_seen[i],ratio)) wide=1;
            }
            if(!p05_raw_policy && decision>blocks && blocks-first_pass>=2*WW_SCORE_HOP_BLOCKS) {
                /* Give a longer word its full observation interval. Start the
                 * normal decision (including cloud verification) early only
                 * after every duration has a complete three-score history and
                 * no word improved at the most recent hop. */
                if(wake_stable_single_candidate(count,blocks*20,max_window,best_seen,gates_now,
                        best_end_ms,best_win,windows,trail)) decision=blocks;
            }
            if(!decision && close && !near_due && blocks>=p05_mute_until)
                near_due=blocks+P05_NEAR_SETTLE_BLOCKS;
            if(!decision && near_due>blocks && !p05_raw_policy && paired &&
               p05_cloud_on() && p05_silence>=12 &&
               blocks>=near_due-P05_NEAR_SETTLE_BLOCKS+12) {
                /* After a quiet tail, start the bounded verification sooner
                 * when only one confirmed word qualifies. */
                int eligible=0, contenders=0;
                for(int i=0;i<count;i++) {
                    if(gates_now[i]<=0.f)continue;
                    float ratio=best_seen[i]/gates_now[i];
                    if(ratio<=1.f+WAKE_NORM_BAND+WAKE_GRAY_MARGIN)++contenders;
                    if(ratio<=1.f+WAKE_NORM_BAND && best_seen[i]<=WAKE_CLOUD_NEAR_CEILING &&
                       best_rms[i]>WW_NORMAL_RMS)++eligible;
                }
                if(eligible==1 && contenders==1)near_due=blocks;
            }
            if(!decision && !near_due && !close && !wide_due && wide && paired && !p05_raw_policy &&
               blocks>=wide_mute_until && !p05_cloud_on()) wide_due=blocks+P05_NEAR_SETTLE_BLOCKS;
            int due=decision ? decision : near_due ? near_due : wide_due;
            if(!due || blocks<due) {
                if(!wake_early_decide(&early,models,count,pcm,used,max_window,blocks,gates_now,&cooldown,&used))
                    continue;
                decision=blocks;
            } else {
            /* Arbitration sees near misses too, not only words that cleared their gate. */
                p05_decide(models,count,best_seen,best_rms,best_win,best_trail,best_end_ms,weak_hit,gates_now,used,blocks,&cooldown,&used);
                if(!decision) {
                    if(near_due) p05_mute_until=blocks+P05_NEAR_WAIT_BLOCKS;
                    wide_mute_until=blocks+P05_NEAR_WAIT_BLOCKS;
                }
            }
        }
        int reset_trails=decision!=0;
        decision=0; first_pass=0; near_due=0; wide_due=0;
        for(int i=0;i<count;i++) {
            pending[i]=best_seen[i]=99; weak_hit[i]=0;
            best_win[i]=best_end_ms[i]=0; best_rms[i]=0;
            memset(best_trail[i],0,sizeof(best_trail[i]));
            if(reset_trails) for(int w=0;w<windows[i];w++)for(int j=0;j<3;j++)trail[i][w][j]=99;
        }
    }
}
#endif

static int detect(int argc, char **argv) {
    float threshold = 0.46f; int argi = 2;
    p05_raw_policy = 0;
#if WW_MFCC == 3
    wake_epoch_input = !strcmp(argv[1], "detect-epoch");
    if(!strcmp(argv[1],"detect-paired")) {
        paired=calloc(1,sizeof(*paired));
        if(!paired) return 3;
    }
#endif
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--threshold") && argi + 1 < argc) {
            threshold = strtof(argv[argi + 1], NULL); argi += 2; continue;
        }
        if (!strcmp(argv[argi], "--policy") && argi + 1 < argc) {
            if (!strcmp(argv[argi + 1], "raw030")) p05_raw_policy = 1;
            else if (!strcmp(argv[argi + 1], "norm")) p05_raw_policy = 0;
            else { fprintf(stderr, "policy must be norm or raw030\n"); return 2; }
            argi += 2; continue;
        }
        break;
    }
    Model models[MAX_MODELS] = {{0}}; int nmodels = 0;
    for (; argi < argc && nmodels < MAX_MODELS; ++argi) {
        if (load_model(&models[nmodels], argv[argi]) != 0) { fprintf(stderr, "cannot load model: %s\n", argv[argi]); return 2; }
        memset(p05_sha[nmodels], 0, sizeof(p05_sha[nmodels]));
        wake_sha256_file(models[nmodels].path, p05_sha[nmodels]);
        ++nmodels;
    }
    if(argi<argc){fprintf(stderr,"too many models (maximum %d)\n",MAX_MODELS);return 2;}
    if (!nmodels) { fprintf(stderr, "no models\n"); return 2; }
    {
        /* Six six-character Chinese labels exceed the old 160-byte buffer;
         * snprintf's would-write count made the next size underflow. Stream
         * diagnostics instead, with no aggregate buffer or truncation. */
        fprintf(stderr, "STARTED models=%d global_threshold=%.3f [",nmodels,threshold);
        for(int i=0;i<nmodels;i++)fprintf(stderr,"%s(%.2f) ",models[i].label,
            models[i].has_threshold?models[i].threshold:threshold);
        fputs("]\n",stderr);
        fflush(stderr);
    }
#if WW_MFCC == 3
    return detect_continuous(models,nmodels,threshold);
#endif
    Segmenter seg = {0}; int16_t block[BLOCK_SAMPLES];
    for (;;) {
        int r = read_block(block); if (r <= 0) return r < 0 ? 3 : 0;
        if (!segment(&seg, block)) continue;
        uint32_t frames = 0; float *feat = extract_features(seg.utterance, seg.utt_samples, &frames);
        if (!feat) continue;
        int best = -1; float best_score = 99.0f, second_score = 99.0f;
        for (int i = 0; i < nmodels; ++i) {
            float sc = score_model(&models[i], feat, (int)frames);
            fprintf(stderr, "SCORE %s %.4f frames=%u\n", models[i].label, sc, frames);
            if (sc < best_score) {
                second_score = best_score; best_score = sc; best = i;
            } else if (sc < second_score) second_score = sc;
        }
        /* 逐词阈值：该词配了 :THRESH 用词的，否则用全局。 */
        float gate = (best >= 0 && models[best].has_threshold)
                     ? models[best].threshold : threshold;
        /* With multiple words, reject an acoustically ambiguous result even if
         * its absolute score passes. This lets us safely use a looser threshold. */
        int separated = nmodels < 2 || second_score - best_score >= 0.060f;
        if (best >= 0 && best_score <= gate && separated) {
            printf("WAKE %s %.4f\n", models[best].label, best_score); fflush(stdout);
            seg.cooldown_blocks = 50; /* 1.0s（官方 agent cooldown 0.7s 量级） */
        } else {
            if (!separated) fprintf(stderr, "AMBIGUOUS margin=%.4f\n", second_score - best_score);
            seg.cooldown_blocks = 20;
        }
        free(feat);
    }
}

/* Diagnostic fixed sliding windows for continuous-background evaluation.
 * Does not emit WAKE or alter production segmentation/thresholds. Compare both
 * features on identical windows before considering a live detector change. */
static int scan_file(int window_ms, int hop_ms, const char *modelarg) {
    if(window_ms<500 || window_ms>2500 || hop_ms<20 || hop_ms%20) return 2;
    Model model={0};
    if(load_model(&model,modelarg)) return 2;
    int samples=window_ms*SAMPLE_RATE/1000, used=0, blocks=0;
    int16_t pcm[MAX_UTT_SAMPLES], block[BLOCK_SAMPLES];
    for(;;) {
        int r=read_block(block); if(r<=0) return r<0 ? 3 : 0;
        ++blocks;
        if(used+BLOCK_SAMPLES>samples) {
            int drop=used+BLOCK_SAMPLES-samples;
            memmove(pcm,pcm+drop,(size_t)(used-drop)*sizeof(int16_t)); used-=drop;
        }
        memcpy(pcm+used,block,sizeof(block)); used+=BLOCK_SAMPLES;
        if(used<samples || blocks%(hop_ms/20)) continue;
        uint32_t frames=0;
        float *feat=extract_features(pcm,used,&frames);
        if(!feat) return 3;
        printf("WINDOW end_ms=%d score=%.6f\n",blocks*20,score_model(&model,feat,(int)frames));
        free(feat);
    }
}

#if WW_MFCC == 3
/* N×N continuous-detection matrix. Each group is LABEL=MODEL:THRESH then wavs,
 * groups separated by --. Four 20ms phases and 1s of silence on both sides. */
static int cmd_matrix(int argc, char **argv) {
    const char *policy = "norm", *report_path = NULL, *detector = NULL;
    FILE *report = NULL;
    int phases = 4, i = 2, ng = 0, total = 0, crosstalk = 0;
    struct { const char *spec, *label; const char **wavs; int n; int hit[8], amb, miss; } g[6];
    char self[512];
    ssize_t self_n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_n <= 0) return 2;
    self[self_n] = 0;
    memset(g, 0, sizeof(g));
    while (i < argc && argv[i][0] == '-' && strcmp(argv[i], "--")) {
        if (!strcmp(argv[i], "--policy") && i + 1 < argc) { policy = argv[++i]; i++; continue; }
        if (!strcmp(argv[i], "--phases") && i + 1 < argc) { phases = atoi(argv[++i]); i++; continue; }
        if (!strcmp(argv[i], "--detect-executable") && i + 1 < argc) { detector = argv[++i]; i++; continue; }
        if (!strcmp(argv[i], "--report-json") && i + 1 < argc) { report_path = argv[++i]; i++; continue; }
        fprintf(stderr, "unknown matrix option %s\n", argv[i]); return 2;
    }
    if (phases < 1 || phases > 4) return 2;
    if (strcmp(policy, "norm") && strcmp(policy, "raw030")) return 2;
    while (i < argc) {
        if (!strcmp(argv[i], "--")) { i++; continue; }
        if (ng == 6) { fprintf(stderr, "matrix supports at most 6 models\n"); return 2; }
        g[ng].spec = argv[i];
        g[ng].label = argv[i];
        g[ng].wavs = (const char **)argv + i + 1;
        i++;
        while (i < argc && strcmp(argv[i], "--")) { g[ng].n++; i++; }
        size_t ln = strcspn(g[ng].spec, "=");
        if (!ln || !g[ng].spec[ln] || ln >= 48 || g[ng].n < 1) {
            fprintf(stderr, "matrix group needs LABEL=MODEL:THRESH and wavs\n"); return 2;
        }
        for (int a = 0; a < ng; a++) if (strcspn(g[a].spec, "=") == ln &&
            !strncmp(g[a].spec, g[ng].spec, ln)) return 2;
        ng++;
    }
    if (ng < 1) return 2;
    if (report_path) {
        int fd = open(report_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) { perror("matrix report"); return 2; }
        report = fdopen(fd, "w");
        if (!report) { close(fd); return 2; }
    }
    for (int a = 0; a < ng; a++) for (int w = 0; w < g[a].n; w++) for (int phase = 0; phase < phases; phase++) {
        int16_t *pcm = NULL; int samples = 0, lead, status = 0;
        int hits[6] = {0}, rejected = 0, wakes = 0, bad_output = 0;
        pid_t pid;
        char line[256];
        /* Private seekable streams avoid the previous fixed 1024-byte output
         * truncation and input/output pipe deadlock on repeated wake events. */
        FILE *input = tmpfile(), *output = tmpfile();
        if (!input || !output) return 2;
        if (read_wav_bounded(g[a].wavs[w], &pcm, &samples, 0) != 0) return 2;
        lead = SAMPLE_RATE + phase * BLOCK_SAMPLES;
        {
            int16_t silence[SAMPLE_RATE + 3 * BLOCK_SAMPLES] = {0};
            int ok = fwrite(silence, 2, (size_t)lead, input) == (size_t)lead &&
                     fwrite(pcm, 2, (size_t)samples, input) == (size_t)samples &&
                     fwrite(silence, 2, SAMPLE_RATE, input) == SAMPLE_RATE;
            free(pcm);
            if (!ok || fflush(input) || fseek(input, 0, SEEK_SET)) return 2;
        }
        pid = fork();
        if (pid < 0) return 2;
        if (!pid) {
            char *args[16]; int k = 0;
            dup2(fileno(input), 0); dup2(fileno(output), 1);
            fclose(input); fclose(output);
            if (report) close(fileno(report));
            setenv("LX06_WAKE_FORENSICS", "0", 1);
            setenv("LX06_WAKE_CLOUD_VERIFY", "off", 1);
            args[k++] = detector ? (char *)detector : self; args[k++] = "detect";
            /* A frozen old detector need not understand this candidate's
             * policy flag. Its exact bytes and own behavior define baseline. */
            if (!detector) { args[k++] = "--policy"; args[k++] = (char *)policy; }
            for (int s = 0; s < ng; s++) args[k++] = (char *)g[s].spec;
            args[k] = NULL;
            execv(args[0], args); _exit(127);
        }
        fclose(input);
        while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return 2; }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fclose(output); fprintf(stderr, "matrix detector failed\n"); return 2;
        }
        rewind(output);
        while (fgets(line, sizeof(line), output)) {
            if (!strchr(line, '\n')) { bad_output = 1; break; }
            if (!strncmp(line, "WAKE ", 5)) {
                char label[48]; int col = -1;
                if (sscanf(line + 5, "%47s", label) != 1) { bad_output = 1; break; }
                for (int s = 0; s < ng; s++) {
                    size_t ln = strcspn(g[s].spec, "=");
                    if (strlen(label) == ln && !strncmp(label, g[s].spec, ln)) col = s;
                }
                if (col < 0) { bad_output = 1; break; }
                hits[col]++; wakes++;
            } else if (!strncmp(line, "DECISION ", 9) &&
                       strncmp(line + 9, "dispatch ", 9) && strncmp(line + 9, "reassign ", 9)) rejected++;
        }
        if (ferror(output)) bad_output = 1;
        fclose(output);
        if (bad_output) return 2;
        total++;
        /* Cells count trials with an event, not just the last event in a clip.
         * A multi-dispatch trial can occupy several columns. JSON keeps counts. */
        for (int s = 0; s < ng; s++) if (hits[s]) {
            g[a].hit[s]++;
            if (s != a) crosstalk += hits[s];
        }
        if (!wakes && rejected) g[a].amb++;
        if (!wakes && !rejected) g[a].miss++;
        if (report) {
            fprintf(report, "{\"row\":%d,\"sample\":%d,\"phase\":%d,\"wake_counts\":[", a, w, phase);
            for (int s = 0; s < ng; s++) fprintf(report, "%s%d", s ? "," : "", hits[s]);
            fprintf(report, "],\"rejection_events\":%d,\"rejected\":%s,\"no_trigger\":%s}\n",
                    rejected, !wakes && rejected ? "true" : "false", !wakes && !rejected ? "true" : "false");
            if (fflush(report) || ferror(report)) return 2;
        }
    }
    if (report && fclose(report)) return 2;
    printf("MATRIX policy=%s id=%s phases=%d clips=%d crosstalk=%d\n", detector ? "frozen-executable" : policy, detector ? "external-bytes" :
           !strcmp(policy,"raw030") ? "raw030-offline-only" : WW_CALIBRATION_POLICY, phases, total, crosstalk);
    for (int a = 0; a < ng; a++) {
        size_t ln = strcspn(g[a].spec, "=");
        printf("ROW %.*s", (int)ln, g[a].spec);
        for (int s = 0; s < ng; s++) {
            size_t sl = strcspn(g[s].spec, "=");
            printf(" %.*s=%d", (int)sl, g[s].spec, g[a].hit[s]);
        }
        printf(" ambiguous=%d miss=%d\n", g[a].amb, g[a].miss);
    }
    printf("SUMMARY crosstalk=%d\n", crosstalk);
    return 0;
}
#endif

static void usage(const char *p) {
    fprintf(stderr,
        "usage:\n"
        "  %s enroll LABEL MODEL [COUNT]\n"
        "  %s detect [--threshold N] LABEL=MODEL[:THRESH]...\n"
        "  %s train LABEL OUT.lwt REC1.wav [REC2.wav ...]\n"
        "  %s capture OUT.wav [MAXSEC]\n"
        "  %s score REC.wav LABEL=MODEL.lwt\n", p, p, p, p, p);
}

int main(int argc, char **argv) {
    /* V42.1: OOM 免死（118MB 整机，10-26 OOM 风暴曾把本检测引擎
     * 当旁观者杀掉 → 检测链无声僵死）。 */
    {
        FILE *oomf = fopen("/proc/self/oom_score_adj", "w");
        if (oomf) { fputs("-1000", oomf); fclose(oomf); }
    }
    init_features();
    if (argc == 2 && !strcmp(argv[1], "info")) {
        printf("FEATURE %s model_version=%d dimension=%d", FEATURE_NAME, MODEL_VERSION, FEAT_DIM);
#if WW_MFCC == 3
        printf(" calibration=%s",WW_CALIBRATION_POLICY);
#endif
        putchar('\n');
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "enroll")) {
        int count = argc > 4 ? atoi(argv[4]) : 3;
        if (count < 1) count = 1; if (count > MAX_TEMPLATES) count = MAX_TEMPLATES;
        return enroll(argv[2], argv[3], count);
    }
    if (argc >= 3 && !strcmp(argv[1], "check")) {
        FILE *f = fopen(argv[2], "rb");
        uint32_t magic = 0, version = 0;
        int ok;
        if (!f) return 1;
        ok = fread(&magic, 4, 1, f) == 1 && fread(&version, 4, 1, f) == 1;
        fclose(f);
        if (!ok || magic != 0x3154574cU || version != MODEL_VERSION) return 1;
        printf("OK\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "detect")) return detect(argc, argv);
#if WW_MFCC == 3
    if (argc >= 3 && (!strcmp(argv[1], "detect-epoch") || !strcmp(argv[1],"detect-paired"))) return detect(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "matrix")) return cmd_matrix(argc, argv);
#endif
    if (argc == 3 && !strcmp(argv[1], "quality")) return enrollment_quality(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "train")) return train(argv[2], argv[3], argc - 4, argv + 4);
    int (*capture_block)(int16_t *) = NULL;
    if (argc >= 3 && !strcmp(argv[1], "capture")) capture_block = read_block;
#if WW_MFCC == 3
    if (argc >= 3 && !strcmp(argv[1], "capture-epoch")) capture_block = read_epoch_capture;
#endif
    if (capture_block) {
        int maxsec = argc > 3 ? atoi(argv[3]) : 8;
        if (maxsec < 1) maxsec = 1; if (maxsec > 30) maxsec = 30;
        return capture(argv[2], maxsec, capture_block, !strcmp(argv[1], "capture-epoch"));
    }
    if (argc >= 4 && !strcmp(argv[1], "score")) return score_file(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "scan")) return scan_file(atoi(argv[2]),atoi(argv[3]),argv[4]);
    usage(argv[0]); return 2;
}
