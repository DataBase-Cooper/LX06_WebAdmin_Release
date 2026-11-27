#include "wake_sample.h"
#include "wake_pinyin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void copy_trunc_utf8(const char *src, char *dst, size_t cap) {
    size_t i = 0;
    if (!src) src = "";
    if (cap < 2) {
        if (cap) dst[0] = 0;
        return;
    }
    while (src[i] && i + 1 < cap) {
        unsigned char c = (unsigned char)src[i];
        size_t need = 1, k;
        if ((c & 0xe0) == 0xc0) need = 2;
        else if ((c & 0xf0) == 0xe0) need = 3;
        else if ((c & 0xf8) == 0xf0) need = 4;
        if (c >= 0x80 && (c & 0xc0) != 0xc0) break;
        if (i + need >= cap) break;
        for (k = 1; k < need; k++) {
            if (((unsigned char)src[i + k] & 0xc0) != 0x80) {
                dst[i] = 0;
                return;
            }
        }
        memcpy(dst + i, src + i, need);
        i += need;
    }
    dst[i] = 0;
}

static void sample_json_escape(const char *src, char *dst, size_t cap) {
    size_t i = 0, j = 0;
    if (!src) src = "";
    if (!cap) return;
    while (src[i] && j + 2 < cap) {
        unsigned char c = (unsigned char)src[i++];
        if (c == '"' || c == '\\') {
            if (j + 3 >= cap) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 3 >= cap) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c < 0x20) continue;
        else dst[j++] = (char)c;
    }
    dst[j] = 0;
}

static int json_get(const char *doc, const char *key, char *out, size_t cap) {
    char pat[48];
    const char *p;
    size_t j = 0;
    if (out && cap) out[0] = 0;
    if (!doc || !key || !out || cap < 2) return -1;
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(doc, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && *p != '"' && j + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[j++] = (*p == 'n') ? '\n' : *p;
            p++;
        } else out[j++] = *p++;
    }
    out[j] = 0;
    return *p == '"' ? 0 : -1;
}

static int status_token(const char *status) {
    size_t n;
    if (!status || !status[0]) return 0;
    n = strlen(status);
    return n < 24 && strspn(status, "abcdefghijklmnopqrstuvwxyz_") == n;
}

static void map_status(const char *rc_text, const char *text, char *pinyin, char *status, int *syllables) {
    pinyin[0] = 0;
    *syllables = 0;
    if (!rc_text) rc_text = "";
    if (!strcmp(rc_text, "too_short") || !strcmp(rc_text, "too_long") ||
        !strcmp(rc_text, "unavailable") || !strcmp(rc_text, "timeout") ||
        !strcmp(rc_text, "cancelled") || !strcmp(rc_text, "empty") ||
        !strcmp(rc_text, "incomplete") || !strcmp(rc_text, "short") ||
        !strcmp(rc_text, "needs_rerecord")) {
        snprintf(status, 24, "%s", rc_text);
        return;
    }
    if (strcmp(rc_text, "0")) {
        if (!strcmp(rc_text, "2")) snprintf(status, 24, "unavailable");
        else if (!strcmp(rc_text, "3")) snprintf(status, 24, "cancelled");
        else if (!strcmp(rc_text, "4")) snprintf(status, 24, "timeout");
        else snprintf(status, 24, "unavailable");
        return;
    }
    if (!text || !text[0]) {
        snprintf(status, 24, "empty");
        return;
    }
    *syllables = wake_pinyin_normalize(text, pinyin, WAKE_PINYIN_CAP);
    if (*syllables < 0) {
        pinyin[0] = 0;
        *syllables = 0;
        snprintf(status, 24, "incomplete");
        return;
    }
    if (*syllables < 2) {
        pinyin[0] = 0;
        snprintf(status, 24, "short");
        return;
    }
    snprintf(status, 24, "ok");
}

int wake_sample_write(const char *json_path, const char *rc_text, const char *asr_text) {
    char text[180], pinyin[WAKE_PINYIN_CAP], status[24];
    char escaped_text[280], escaped_py[WAKE_PINYIN_CAP * 2];
    char tmp[320], body[768];
    int syllables = 0;
    FILE *file;
    if (!json_path || !json_path[0] || strstr(json_path, "..") || strlen(json_path) > 240) return -1;
    copy_trunc_utf8(asr_text, text, sizeof(text));
    map_status(rc_text, text, pinyin, status, &syllables);
    if (strcmp(status, "ok")) {
        pinyin[0] = 0;
        syllables = 0;
    }
    sample_json_escape(text, escaped_text, sizeof(escaped_text));
    sample_json_escape(pinyin, escaped_py, sizeof(escaped_py));
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"pinyin\":\"%s\",\"status\":\"%s\",\"syllables\":%d}\n",
             escaped_text, escaped_py, status, syllables);
    snprintf(tmp, sizeof(tmp), "%s.tmp", json_path);
    file = fopen(tmp, "w");
    if (!file) return -1;
    if (fputs(body, file) == EOF || fclose(file) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, json_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int read_sidecar(const char *path, char *text, size_t text_cap, char *pinyin, size_t py_cap,
                        char *status, size_t status_cap) {
    FILE *file;
    char body[640];
    size_t n;
    if (text && text_cap) text[0] = 0;
    if (pinyin && py_cap) pinyin[0] = 0;
    if (status && status_cap) status[0] = 0;
    file = fopen(path, "r");
    if (!file) return -1;
    n = fread(body, 1, sizeof(body) - 1, file);
    fclose(file);
    body[n] = 0;
    if (json_get(body, "text", text, text_cap) != 0 && text && text_cap) text[0] = 0;
    if (json_get(body, "pinyin", pinyin, py_cap) != 0 && pinyin && py_cap) pinyin[0] = 0;
    if (json_get(body, "status", status, status_cap) != 0) return -1;
    if (!status_token(status)) return -1;
    return 0;
}

int wake_samples_load(const char *recdir, struct wake_cloud_word *word) {
    int n;
    if (!word) return -1;
    word->nsamples = 0;
    word->missing = 0;
    if (!recdir || !recdir[0] || strstr(recdir, "..")) return -1;
    for (n = 1; n <= WAKE_SAMPLE_MAX; n++) {
        char wav[320], js[320], text[180], pinyin[WAKE_PINYIN_CAP], status[32];
        int have_wav, have_js, usable;
        snprintf(wav, sizeof(wav), "%s/%d.wav", recdir, n);
        snprintf(js, sizeof(js), "%s/%d.asr.json", recdir, n);
        have_wav = access(wav, F_OK) == 0;
        have_js = access(js, F_OK) == 0;
        if (!have_wav && !have_js) continue;
        usable = have_js &&
                 read_sidecar(js, text, sizeof(text), pinyin, sizeof(pinyin), status, sizeof(status)) == 0 &&
                 !strcmp(status, "ok") && wake_pinyin_key_equal(pinyin, pinyin);
        if (!usable) {
            word->missing++;
            continue;
        }
        if (word->nsamples < WAKE_SAMPLE_MAX) {
            snprintf(word->sample_pinyin[word->nsamples], WAKE_PINYIN_CAP, "%s", pinyin);
            word->nsamples++;
        }
    }
    return 0;
}

int wake_samples_json(const char *recdir, char *out, size_t cap) {
    int n, first = 1;
    size_t used;
    if (!out || cap < 3) return -1;
    if (!recdir || strstr(recdir, "..")) {
        snprintf(out, cap, "[]");
        return 0;
    }
    used = (size_t)snprintf(out, cap, "[");
    if (used >= cap) return -1;
    for (n = 1; n <= WAKE_SAMPLE_MAX; n++) {
        char wav[320], js[320], text[180], pinyin[WAKE_PINYIN_CAP], status[32];
        char etext[280], epinyin[WAKE_PINYIN_CAP * 2], piece[640];
        int piece_n;
        snprintf(wav, sizeof(wav), "%s/%d.wav", recdir, n);
        if (access(wav, F_OK) != 0) continue;
        snprintf(js, sizeof(js), "%s/%d.asr.json", recdir, n);
        text[0] = pinyin[0] = 0;
        snprintf(status, sizeof(status), "needs_rerecord");
        if (read_sidecar(js, text, sizeof(text), pinyin, sizeof(pinyin), status, sizeof(status)) != 0 ||
            !status_token(status)) {
            text[0] = pinyin[0] = 0;
            snprintf(status, sizeof(status), "needs_rerecord");
        }
        if (strcmp(status, "ok")) pinyin[0] = 0;
        sample_json_escape(text, etext, sizeof(etext));
        sample_json_escape(pinyin, epinyin, sizeof(epinyin));
        piece_n = snprintf(piece, sizeof(piece),
                           "%s{\"n\":%d,\"text\":\"%s\",\"pinyin\":\"%s\",\"status\":\"%s\"}",
                           first ? "" : ",", n, etext, epinyin, status);
        if (piece_n < 0 || used + (size_t)piece_n + 2 >= cap) {
            snprintf(out, cap, "[]");
            return -1;
        }
        memcpy(out + used, piece, (size_t)piece_n);
        used += (size_t)piece_n;
        out[used] = 0;
        first = 0;
    }
    if (used + 2 >= cap) {
        snprintf(out, cap, "[]");
        return -1;
    }
    out[used++] = ']';
    out[used] = 0;
    return 0;
}

#ifdef WAKE_SAMPLE_TOOL
int main(int argc, char **argv) {
    char text[240];
    size_t n = 0;
    char *nl;
    if (argc != 3) {
        fprintf(stderr, "usage: wake_sample_commit JSON RC\n");
        return 2;
    }
    while (n + 1 < sizeof(text)) {
        ssize_t got = read(STDIN_FILENO, text + n, sizeof(text) - 1 - n);
        if (got == 0) break;
        if (got < 0) return 2;
        n += (size_t)got;
    }
    text[n] = 0;
    nl = strchr(text, '\n');
    if (nl) *nl = 0;
    if (wake_sample_write(argv[1], argv[2], text) != 0) return 2;
    return 0;
}
#endif
