#include "wake_pinyin.h"
#include "wake_pinyin_table.h"
#include "wake_policy.h"
#include <string.h>

static int wake_py_lookup(unsigned cp, const char **syllable) {
    int lo = 0, hi = WAKE_PY_CHARS - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        unsigned at = wake_py_cp[mid];
        if (at == cp) {
            *syllable = wake_py_syllables[wake_py_id[mid]];
            return 1;
        }
        if (at < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static int utf8_next(const char **text, unsigned *cp) {
    const unsigned char *p = (const unsigned char *)*text;
    if (!*p) return 0;
    if (*p < 0x80) { *cp = *p; *text += 1; return 1; }
    if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        *cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *text += 2; return 1;
    }
    if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        *cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *text += 3; return 1;
    }
    if ((*p & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        *cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        *text += 4; return 1;
    }
    *text += 1; *cp = 0; return 1;
}

int wake_pinyin_phrase(const char *utf8, char *out, size_t cap) {
    const char *p = utf8 ? utf8 : "";
    size_t used = 0;
    int count = 0, missing = 0;
    if (out && cap) out[0] = 0;
    while (*p) {
        unsigned cp = 0;
        const char *syllable = NULL;
        if (!utf8_next(&p, &cp)) break;
        if (cp < 0x80 && (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r')) continue;
        if (cp < 0x80) { missing = 1; continue; }
        if (!wake_py_lookup(cp, &syllable)) { missing = 1; continue; }
        if (out && cap) {
            size_t n = strlen(syllable);
            if (used && used + 1 < cap) out[used++] = ' ';
            if (used + n >= cap) { if (cap) out[cap - 1] = 0; return -1; }
            memcpy(out + used, syllable, n);
            used += n;
            out[used] = 0;
        }
        count++;
    }
    return missing ? -1 : count;
}

static void split_syllable(const char *syllable, char *initial, char *final) {
    static const char *initials[] = {
        "zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h",
        "j", "q", "x", "r", "z", "c", "s", "y", "w", NULL
    };
    int i;
    initial[0] = 0;
    strcpy(final, syllable);
    for (i = 0; initials[i]; i++) {
        size_t n = strlen(initials[i]);
        if (!strncmp(syllable, initials[i], n) && syllable[n]) {
            memcpy(initial, initials[i], n + 1);
            strcpy(final, syllable + n);
            return;
        }
    }
}

static int next_syllable(const char **p, char *out, size_t cap) {
    const char *s = *p;
    size_t n = 0;
    while (*s == ' ') s++;
    if (!*s) { *p = s; return 0; }
    while (s[n] && s[n] != ' ' && n + 1 < cap) { out[n] = s[n]; n++; }
    out[n] = 0;
    *p = s + n;
    return 1;
}

static int reduplicated(const char *phrase) {
    char a[8][8];
    int n = 0;
    const char *p = phrase;
    while (n < 8 && next_syllable(&p, a[n], sizeof(a[n]))) n++;
    if (n < 4 || (n % 2)) return 0;
    for (int i = 0; i < n / 2; i++) if (strcmp(a[i], a[i + n / 2])) return 0;
    return 1;
}

float wake_pinyin_warn_similarity(const char *left, const char *right) {
    char ls[96], rs[96], a[8][8], b[8][8];
    int na = 0, nb = 0, pairs, i;
    float sum = 0;
    if (wake_pinyin_phrase(left, ls, sizeof(ls)) <= 0) return 0;
    if (wake_pinyin_phrase(right, rs, sizeof(rs)) <= 0) return 0;
    const char *pa = ls, *pb = rs;
    while (na < 8 && next_syllable(&pa, a[na], sizeof(a[na]))) na++;
    while (nb < 8 && next_syllable(&pb, b[nb], sizeof(b[nb]))) nb++;
    pairs = na > nb ? na : nb;
    if (!pairs) return 0;
    for (i = 0; i < na && i < nb; i++) {
        char ia[8], fa[8], ib[8], fb[8];
        float part = 0;
        split_syllable(a[i], ia, fa);
        split_syllable(b[i], ib, fb);
        if (!strcmp(ia, ib)) part += 0.4f;
        if (!strcmp(fa, fb)) part += 0.6f;
        sum += part;
    }
    sum /= (float)pairs;
    if (reduplicated(ls) && reduplicated(rs) && sum < 1.0f) sum += 0.15f;
    if (sum > 1.0f) sum = 1.0f;
    return sum;
}

static void normalize_asr(char *syllable) {
    char repl = 0;
    if (!strncmp(syllable, "zh", 2) && syllable[2]) repl = 'z';
    else if (!strncmp(syllable, "ch", 2) && syllable[2]) repl = 'c';
    else if (!strncmp(syllable, "sh", 2) && syllable[2]) repl = 's';
    else return;
    syllable[0] = repl;
    memmove(syllable + 1, syllable + 2, strlen(syllable + 2) + 1);
}

static int load_norm(const char *phrase, char out[][8], int maxn) {
    char buf[96];
    const char *p = buf;
    int n = 0, rc = wake_pinyin_phrase(phrase, buf, sizeof(buf));
    if (rc == 0) return 0;
    while (n < maxn && next_syllable(&p, out[n], 8)) { normalize_asr(out[n]); n++; }
    return n;
}

static int is_ideograph(unsigned cp) {
    return cp >= 0x4e00 && cp <= 0x9fff;
}

int wake_pinyin_normalize(const char *utf8, char *out, size_t cap) {
    const char *p = utf8 ? utf8 : "";
    size_t used = 0;
    int count = 0, missing = 0;
    if (out && cap) out[0] = 0;
    while (*p) {
        unsigned cp = 0;
        const char *syllable = NULL;
        if (!utf8_next(&p, &cp)) break;
        if (!is_ideograph(cp)) continue;
        if (!wake_py_lookup(cp, &syllable) || !syllable || !syllable[0]) {
            missing = 1;
            continue;
        }
        if (out && cap) {
            size_t n = strlen(syllable);
            if (used && used + 1 < cap) out[used++] = ' ';
            if (used + n >= cap) {
                if (cap) out[cap - 1] = 0;
                return -1;
            }
            memcpy(out + used, syllable, n);
            used += n;
            out[used] = 0;
        }
        count++;
    }
    return missing ? -1 : count;
}

static int key_syllables(const char *key) {
    int gaps = 0, syl = 0;
    const char *p;
    if (!key || !key[0]) return 0;
    for (p = key; *p; p++) {
        if (*p == ' ') {
            if (syl < 1) return -1;
            syl = 0;
            gaps++;
        } else if (*p < 'a' || *p > 'z') return -1;
        else syl++;
    }
    if (syl < 1) return -1;
    return gaps + 1;
}

int wake_pinyin_key_equal(const char *left, const char *right) {
    int a = key_syllables(left), b = key_syllables(right);
    if (a < 2 || b < 2 || a != b) return 0;
    return strcmp(left, right) == 0;
}

int wake_pinyin_asr_match(const char *heard, const char *enrolled) {
    char a[8][8], b[8][8];
    int na = load_norm(heard, a, 8), nb = load_norm(enrolled, b, 8), i, matched = 0;
    if (na < 2 || nb < 2) return 0;
    if (na == nb) {
        for (i = 0; i < na; i++) if (!strcmp(a[i], b[i])) matched++;
        /* 2/3 covers 古维斯↔贾维斯; an exact 小吱小吱↔小智小智 scores 1. */
        if (matched * 3 >= na * 2) return 1;
    }
    if (na <= nb) {
        for (i = 0; i + na <= nb; i++) {
            int k, ok = 1;
            for (k = 0; k < na; k++) if (strcmp(a[k], b[i + k])) { ok = 0; break; }
            if (ok && na * 2 >= nb) return 1;
        }
    }
    return 0;
}
