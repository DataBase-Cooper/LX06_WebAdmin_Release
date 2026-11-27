#include "template-matcher.h"
#include <stdio.h>
#include <string.h>

int lx06_phrase_normalize(const char *in, char *out, size_t cap) {
    size_t n = 0;
    if (!in || !out || cap < 2) return 0;
    while (*in) {
        unsigned char c = (unsigned char)*in;
        if (c < 128 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || strchr(".,!?;:", c))) {
            in++;
            continue;
        }
        {
            const char *p[] = {"，", "。", "！", "？", "；", "：", "、", "　"};
            unsigned i;
            int skip = 0;
            for (i = 0; i < sizeof(p) / sizeof(*p); i++) {
                size_t k = strlen(p[i]);
                if (!strncmp(in, p[i], k)) { in += k; skip = 1; break; }
            }
            if (skip) continue;
        }
        if (c < 32 || n + 1 >= cap) return 0;
        out[n++] = *in++;
    }
    out[n] = 0;
    return n > 0;
}

static int utf8_len(const unsigned char *s) {
    if ((*s & 0x80) == 0) return 1;
    if ((*s & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) return 2;
    if ((*s & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) return 3;
    if ((*s & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) return 4;
    return 0;
}

static int char_count(const char *s, int bytes) {
    int n = 0, i = 0;
    if (bytes < 0) return -1;
    while (i < bytes) {
        int k = utf8_len((const unsigned char *)s + i);
        if (k <= 0 || i + k > bytes) return -1;
        i += k;
        n++;
    }
    return n;
}

static int var_limit(const char *name) {
    if (!strcmp(name, "artist") || !strcmp(name, "song")) return 32;
    return 64;
}

struct lx06_seg {
    int var;
    char lit[160];
    int lit_bytes;
    char name[17];
    int vmin, vmax;
};

struct lx06_pat {
    int ok, segs, vars, fixed;
    char skeleton[160];
    struct lx06_seg seg[8];
};

static int script_same(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    return !strcmp(a, b);
}

static int parse_pat(const char *phrase, struct lx06_pat *pat) {
    const char *p = phrase;
    char norm[160];
    size_t n = 0;
    memset(pat, 0, sizeof(*pat));
    if (!phrase || !*phrase || strlen(phrase) >= sizeof(norm)) return 0;
    /* Strip spaces and punctuation in the literal parts only. */
    while (*p) {
        if (*p == '{') {
            const char *e = strchr(p + 1, '}');
            size_t name_n;
            char name[17];
            if (!e || e == p + 1) return 0;
            name_n = (size_t)(e - (p + 1));
            if (name_n < 1 || name_n > 16) return 0;
            memcpy(name, p + 1, name_n);
            name[name_n] = 0;
            for (size_t i = 0; i < name_n; i++)
                if (!((name[i] >= 'a' && name[i] <= 'z') || name[i] == '_')) return 0;
            if (n + name_n + 2 >= sizeof(norm)) return 0;
            norm[n++] = '{';
            memcpy(norm + n, name, name_n);
            n += name_n;
            norm[n++] = '}';
            p = e + 1;
            continue;
        }
        {
            unsigned char c = (unsigned char)*p;
            if (c < 128 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || strchr(".,!?;:", c))) { p++; continue; }
            {
                const char *marks[] = {"，", "。", "！", "？", "；", "：", "、", "　"};
                unsigned i; int skip = 0;
                for (i = 0; i < sizeof(marks) / sizeof(*marks); i++) {
                    size_t k = strlen(marks[i]);
                    if (!strncmp(p, marks[i], k)) { p += k; skip = 1; break; }
                }
                if (skip) continue;
            }
            if (c < 32 || n + 1 >= sizeof(norm)) return 0;
            norm[n++] = *p++;
        }
    }
    norm[n] = 0;
    if (!n) return 0;
    p = norm;
    while (*p) {
        struct lx06_seg *seg;
        if (pat->segs == 8) return 0;
        seg = &pat->seg[pat->segs++];
        if (*p == '{') {
            const char *e = strchr(p, '}');
            size_t name_n = (size_t)(e - (p + 1));
            seg->var = 1;
            memcpy(seg->name, p + 1, name_n);
            seg->name[name_n] = 0;
            seg->vmin = 1;
            seg->vmax = var_limit(seg->name);
            pat->vars++;
            p = e + 1;
        } else {
            size_t k = 0;
            seg->var = 0;
            while (p[k] && p[k] != '{') k++;
            if (k >= sizeof(seg->lit)) return 0;
            memcpy(seg->lit, p, k);
            seg->lit[k] = 0;
            seg->lit_bytes = (int)k;
            {
                int chars = char_count(seg->lit, seg->lit_bytes);
                if (chars < 0) return 0;
                pat->fixed += chars;
                if (strlen(pat->skeleton) + k >= sizeof(pat->skeleton)) return 0;
                strcat(pat->skeleton, seg->lit);
            }
            p += k;
        }
    }
    if (pat->vars && pat->fixed <= 0) return 0;
    if (pat->vars > 4) return 0;
    pat->ok = 1;
    return 1;
}

static int match_from(const char *text, int at, int bytes, const struct lx06_pat *pat, int si,
                      struct lx06_match_var *vars, int *var_count) {
    const struct lx06_seg *seg;
    if (si == pat->segs) return at == bytes;
    seg = &pat->seg[si];
    if (!seg->var) {
        if (at + seg->lit_bytes > bytes || memcmp(text + at, seg->lit, (size_t)seg->lit_bytes)) return 0;
        return match_from(text, at + seg->lit_bytes, bytes, pat, si + 1, vars, var_count);
    }
    if (si + 1 == pat->segs) {
        int chars = char_count(text + at, bytes - at);
        if (chars < seg->vmin || chars > seg->vmax) return 0;
        if (bytes - at >= (int)sizeof(vars[0].value)) return 0;
        memset(&vars[*var_count], 0, sizeof(vars[0]));
        memcpy(vars[*var_count].name, seg->name, strlen(seg->name));
        memcpy(vars[*var_count].value, text + at, (size_t)(bytes - at));
        (*var_count)++;
        return 1;
    }
    {
        int taken = 0, i = at;
        while (i < bytes && taken < seg->vmax) {
            int k = utf8_len((const unsigned char *)text + i);
            struct lx06_match_var saved[4];
            int saved_n;
            if (k <= 0 || i + k > bytes) return 0;
            i += k;
            taken++;
            if (taken < seg->vmin) continue;
            saved_n = *var_count;
            memcpy(saved, vars, sizeof(saved));
            if (i - at >= (int)sizeof(vars[0].value)) return 0;
            memset(&vars[*var_count], 0, sizeof(vars[0]));
            memcpy(vars[*var_count].name, seg->name, strlen(seg->name));
            memcpy(vars[*var_count].value, text + at, (size_t)(i - at));
            (*var_count)++;
            if (match_from(text, i, bytes, pat, si + 1, vars, var_count)) return 1;
            *var_count = saved_n;
            memcpy(vars, saved, sizeof(saved));
        }
    }
    return 0;
}

int lx06_template_match(const char *text, const struct lx06_match_candidate *cands,
                        int count, struct lx06_match_result *out) {
    char norm[128];
    int i, best_fixed = -1, best_index = -1, best_line = 0, scripts = 0;
    const char *best_script = "";
    struct lx06_match_var best_vars[4];
    int best_var_count = 0, incomplete = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->index = -1;
    if (!text || count < 0 || (count > 0 && !cands)) return -1;
    if (count > 128) count = 128;
    if (!lx06_phrase_normalize(text, norm, sizeof(norm))) return 0;
    for (i = 0; i < count; i++) {
        char phrase[128];
        if (!cands[i].phrase || strchr(cands[i].phrase, '{')) continue;
        if (!lx06_phrase_normalize(cands[i].phrase, phrase, sizeof(phrase))) continue;
        if (strcmp(phrase, norm)) continue;
        if (best_index < 0 || !script_same(cands[i].script, best_script)) {
            if (best_index >= 0 && !script_same(cands[i].script, best_script)) scripts++;
            else scripts = 1;
        }
        if (best_index < 0 || !script_same(cands[i].script, best_script) || cands[i].line < best_line) {
            /* Keep the first script's earliest line until a second script appears. */
            if (best_index < 0 || script_same(cands[i].script, best_script)) {
                best_index = i;
                best_line = cands[i].line;
                best_script = cands[i].script ? cands[i].script : "";
            }
        }
        if (scripts > 1) {
            out->kind = LX06_MATCH_AMBIGUOUS;
            out->index = -1;
            return 0;
        }
    }
    if (best_index >= 0 && scripts == 1) {
        out->kind = LX06_MATCH_EXACT;
        out->index = best_index;
        return 0;
    }
    best_index = -1;
    best_fixed = -1;
    scripts = 0;
    best_script = "";
    for (i = 0; i < count; i++) {
        struct lx06_pat pat;
        struct lx06_match_var vars[4];
        int var_count = 0;
        if (!cands[i].phrase || !strchr(cands[i].phrase, '{')) continue;
        if (!parse_pat(cands[i].phrase, &pat)) continue;
        if (!match_from(norm, 0, (int)strlen(norm), &pat, 0, vars, &var_count)) {
            if (pat.vars && !strcmp(norm, pat.skeleton)) incomplete = 1;
            continue;
        }
        if (pat.fixed > best_fixed) {
            best_fixed = pat.fixed;
            best_index = i;
            best_line = cands[i].line;
            best_script = cands[i].script ? cands[i].script : "";
            best_var_count = var_count;
            memcpy(best_vars, vars, sizeof(best_vars));
            scripts = 1;
        } else if (pat.fixed == best_fixed) {
            if (!script_same(cands[i].script, best_script)) {
                scripts++;
            } else if (cands[i].line < best_line) {
                best_index = i;
                best_line = cands[i].line;
                best_var_count = var_count;
                memcpy(best_vars, vars, sizeof(best_vars));
            }
        }
    }
    if (scripts > 1) {
        out->kind = LX06_MATCH_REJECT;
        out->fixed_chars = best_fixed;
        snprintf(out->prompt, sizeof(out->prompt), "%s", LX06_PROMPT_REJECT);
        return 0;
    }
    if (best_index >= 0) {
        out->kind = LX06_MATCH_TEMPLATE;
        out->index = best_index;
        out->fixed_chars = best_fixed;
        out->var_count = best_var_count;
        memcpy(out->vars, best_vars, sizeof(out->vars));
        return 0;
    }
    if (incomplete) {
        out->kind = LX06_MATCH_INCOMPLETE;
        snprintf(out->prompt, sizeof(out->prompt), "%s", LX06_PROMPT_REASK);
        return 0;
    }
    out->kind = LX06_MATCH_NONE;
    return 0;
}
