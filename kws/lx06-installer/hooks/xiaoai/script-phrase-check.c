#define _GNU_SOURCE
#include "script-phrase-check.h"
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int line_at(const char *header, size_t n, size_t *at, const char **start, size_t *len) {
    size_t i = *at, k;
    if (i >= n) return 0;
    k = i;
    while (k < n && header[k] && header[k] != '\n') k++;
    *start = header + i;
    *len = k - i;
    while (*len && ((*start)[*len - 1] == '\r' || (*start)[*len - 1] == ' ' || (*start)[*len - 1] == '\t'))
        (*len)--;
    *at = k < n && header[k] == '\n' ? k + 1 : n;
    return 1;
}

static int starts(const char *s, size_t n, const char *key) {
    size_t k = strlen(key);
    return n >= k && !memcmp(s, key, k);
}

static int bridge_header(const char *header, size_t n) {
    size_t at = 0;
    const char *s; size_t len;
    while (line_at(header, n, &at, &s, &len)) {
        if (starts(s, len, "# LX06-BRIDGE: on") || starts(s, len, "# LX06-AUDIO-MODE: phone-bridge") ||
            starts(s, len, "# LX06-PHONE-APPS:"))
            return 1;
    }
    return 0;
}

static int value_nonempty(const char *s, size_t n, const char *key) {
    size_t k = strlen(key);
    const char *v;
    size_t vn;
    if (n < k || memcmp(s, key, k)) return 0;
    v = s + k;
    vn = n - k;
    while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
    return vn > 0;
}

static int find_value(const char *header, size_t n, const char *key) {
    size_t at = 0, kn = strlen(key);
    const char *s; size_t len;
    while (line_at(header, n, &at, &s, &len)) {
        if (len >= kn && !memcmp(s, key, kn)) return value_nonempty(s, len, key);
    }
    return -1; /* key absent */
}

static int audio_mic_open(const char *header, size_t n, const char *app) {
    char key[80];
    size_t at = 0;
    const char *s; size_t len;
    {
        char app_up[17];
        size_t ai;
        for (ai = 0; app && app[ai] && ai < sizeof(app_up) - 1; ai++) {
            char c = app[ai];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            app_up[ai] = c;
        }
        app_up[ai] = 0;
        snprintf(key, sizeof(key), "# LX06-%s-AUDIO:", app_up);
    }
    while (line_at(header, n, &at, &s, &len)) {
        if (starts(s, len, key)) {
            const char *hit = "mic=open";
            size_t hn = strlen(hit);
            if (len >= hn) {
                for (size_t i = 0; i + hn <= len; i++)
                    if (!memcmp(s + i, hit, hn)) return 1;
            }
        }
    }
    return 0;
}

int lx06_exit_phrases_ok(const char *header, size_t n, int official_on, const char **error) {
    char apps[128];
    const char *s; size_t len, at = 0;
    if (error) *error = NULL;
    if (!header || official_on) return 0;
    if (n > 8192) n = 8192;
    if (!bridge_header(header, n)) return 0;
    apps[0] = 0;
    while (line_at(header, n, &at, &s, &len)) {
        const char *key = "# LX06-PHONE-APPS:";
        size_t kn = strlen(key);
        if (len >= kn && !memcmp(s, key, kn)) {
            const char *v = s + kn;
            size_t vn = len - kn;
            while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
            if (vn >= sizeof(apps)) vn = sizeof(apps) - 1;
            memcpy(apps, v, vn);
            apps[vn] = 0;
            break;
        }
    }
    if (!apps[0]) {
        if (error) *error = "桥脚本在官方唤醒关闭时必须声明手机应用和结束说法";
        return 1;
    }
    {
        char *save = NULL, *tok = strtok_r(apps, ",", &save);
        while (tok) {
            char hang[80], exitk[80];
            int present;
            while (*tok == ' ') tok++;
            if (!*tok || strlen(tok) > 16) {
                if (error) *error = "手机应用名不合法";
                return 1;
            }
            {
                char app[17];
                size_t ai;
                for (ai = 0; tok[ai] && ai < sizeof(app) - 1; ai++) {
                    char c = tok[ai];
                    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                    app[ai] = c;
                }
                app[ai] = 0;
                snprintf(hang, sizeof(hang), "# LX06-HANGUP-%s:", app);
                snprintf(exitk, sizeof(exitk), "# LX06-EXIT-%s:", app);
            }
            if (audio_mic_open(header, n, tok)) {
                present = find_value(header, n, hang);
                if (present <= 0) {
                    if (error) *error = "官方唤醒关闭时，语音应用的挂断词不能留空";
                    return 1;
                }
            } else {
                present = find_value(header, n, exitk);
                if (present <= 0) {
                    if (error) *error = "官方唤醒关闭时，操作应用的退出词不能留空";
                    return 1;
                }
            }
            tok = strtok_r(NULL, ",", &save);
        }
    }
    return 0;
}

static int collect(const char *header, size_t n, const char *script,
                   struct lx06_match_candidate *out, int max, int *count) {
    size_t at = 0;
    const char *s; size_t len;
    int line = 0;
    while (line_at(header, n, &at, &s, &len) && *count < max) {
        const char *keys[] = {"# 小爱通道唤醒词=", "# 小爱通道指令=", "# LX06-LOCAL-CHANNEL:"};
        unsigned k;
        const char *val = NULL;
        size_t vn = 0;
        char *buf, *save, *tok;
        int pipe = 0;
        for (k = 0; k < 3; k++) {
            size_t kn = strlen(keys[k]);
            if (len > kn && !memcmp(s, keys[k], kn)) {
                val = s + kn; vn = len - kn; pipe = (k == 2); break;
            }
        }
        if (!val && len > 8 && !memcmp(s, "# LX06-", 7) && memchr(s, '{', len)) {
            const char *colon = memchr(s, ':', len);
            if (colon && colon + 1 < s + len) {
                val = colon + 1;
                vn = (size_t)((s + len) - val);
                pipe = 1;
            }
        }
        if (!val) continue;
        buf = strndup(val, vn);
        if (!buf) return -1;
        for (char *p = buf; pipe && (p = strchr(p, '|')); ) *p = ',';
        tok = strtok_r(buf, ",", &save);
        while (tok && *count < max) {
            while (*tok == ' ') tok++;
            if (*tok) {
                out[*count].script = script;
                out[*count].line = line++;
                out[*count].phrase = strdup(tok);
                if (!out[*count].phrase) { free(buf); return -1; }
                (*count)++;
            }
            tok = strtok_r(NULL, ",", &save);
        }
        free(buf);
    }
    return 0;
}

static void free_cands(struct lx06_match_candidate *c, int n) {
    for (int i = 0; i < n; i++) free((void *)c[i].phrase);
}

int lx06_phrase_conflicts(const char *header, size_t n, const char *name, const char *apps_dir) {
    struct lx06_match_candidate mine[64], other[64];
    int mine_n = 0, other_n = 0, conflict = 0;
    DIR *d;
    struct dirent *de;
    if (!header || !name || !apps_dir) return 0;
    if (n > 8192) n = 8192;
    if (collect(header, n, name, mine, 64, &mine_n) != 0) { free_cands(mine, mine_n); return 1; }
    if (!mine_n) return 0;
    d = opendir(apps_dir);
    if (!d) { free_cands(mine, mine_n); return 0; }
    while (!conflict && (de = readdir(d))) {
        char path[320];
        char buf[8192];
        int fd; ssize_t got;
        if (de->d_name[0] == '.' || !strcmp(de->d_name, name)) continue;
        if (snprintf(path, sizeof(path), "%s/%s", apps_dir, de->d_name) >= (int)sizeof(path)) continue;
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        got = read(fd, buf, sizeof(buf));
        close(fd);
        if (got <= 0) continue;
        other_n = 0;
        if (collect(buf, (size_t)got, de->d_name, other, 64, &other_n) != 0) { conflict = 1; free_cands(other, other_n); break; }
        for (int i = 0; i < mine_n && !conflict; i++) {
            struct lx06_match_candidate pair[2];
            struct lx06_match_result mr;
            char probe[160];
            pair[0] = mine[i];
            pair[1] = other[0];
            for (int j = 0; j < other_n && !conflict; j++) {
                pair[1] = other[j];
                if (!strchr(mine[i].phrase, '{') && !strchr(other[j].phrase, '{')) {
                    char a[128], b[128];
                    if (lx06_phrase_normalize(mine[i].phrase, a, sizeof(a)) &&
                        lx06_phrase_normalize(other[j].phrase, b, sizeof(b)) && !strcmp(a, b))
                        conflict = 1;
                    continue;
                }
                /* Probe each template with a one-character variable. */
                if (strchr(mine[i].phrase, '{')) {
                    const char *p = mine[i].phrase;
                    size_t o = 0;
                    while (*p && o + 4 < sizeof(probe)) {
                        if (*p == '{') {
                            const char *e = strchr(p, '}');
                            if (!e) break;
                            probe[o++] = '\xe6'; probe[o++] = '\xb5'; probe[o++] = '\x8b'; /* 测 */
                            p = e + 1;
                        } else probe[o++] = *p++;
                    }
                    probe[o] = 0;
                    if (lx06_template_match(probe, pair, 2, &mr) == 0 && mr.kind == LX06_MATCH_REJECT)
                        conflict = 1;
                }
            }
        }
        free_cands(other, other_n);
    }
    closedir(d);
    free_cands(mine, mine_n);
    return conflict;
}
