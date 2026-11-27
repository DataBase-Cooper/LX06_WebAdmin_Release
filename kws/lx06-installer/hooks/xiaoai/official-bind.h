/* Device-level official wake binding (§2.9). The value stays on the speaker.
 * It is not a script header and must not be committed. Empty means today's
 * behavior: 「小爱同学」 stays with the official assistant, and
 * 「小爱同学」+ a channel phrase still routes. */
#ifndef LX06_OFFICIAL_BIND_H
#define LX06_OFFICIAL_BIND_H
#include "channel-route.h"
#ifndef LX06_SCRIPTS_ALLOWED_EXTERNAL
#include "../robust/script-policy.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef LX06_OFFICIAL_BIND_PATH
#define LX06_OFFICIAL_BIND_PATH "/data/wakeword/official_bind"
#endif
#ifndef LX06_OFFICIAL_BIND_EVENT
#define LX06_OFFICIAL_BIND_EVENT "/tmp/lx06-official-bind-wake"
#endif
#ifndef LX06_OFFICIAL_SWITCH_PATH
#define LX06_OFFICIAL_SWITCH_PATH "/data/wakeword/official_xiaoai"
#endif
#ifndef LX06_BIND_APPS
#define LX06_BIND_APPS "/data/lx06/apps"
#endif

static inline int lx06_bind_name_ok(const char *s) {
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (!n || n > 64 || s[0] == '.') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Missing file = on. Only a leading 0 turns the official keyword off. */
static inline int lx06_official_kws_enabled_at(const char *path) {
    char c = '1';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    if (read(fd, &c, 1) < 0) c = '1';
    close(fd);
    return c != '0';
}

static inline int lx06_official_kws_enabled(void) {
    return lx06_official_kws_enabled_at(LX06_OFFICIAL_SWITCH_PATH);
}

/* 1 = a script id was stored, 0 = empty or missing, -1 = corrupt. */
static inline int lx06_official_bind_read_at(const char *path, char *out, size_t cap) {
    char buf[80];
    ssize_t n;
    int fd;
    if (!out || cap < 2) return -1;
    out[0] = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    if (!buf[0]) return 0;
    if (!lx06_bind_name_ok(buf) || (size_t)n >= cap) return -1;
    memcpy(out, buf, (size_t)n + 1);
    return 1;
}

static inline int lx06_official_bind_read(char *out, size_t cap) {
    return lx06_official_bind_read_at(LX06_OFFICIAL_BIND_PATH, out, cap);
}

static inline int lx06_official_bind_write_at(const char *path, const char *id) {
    char tmp[256];
    int fd, ok;
    if (id && *id && !lx06_bind_name_ok(id)) return -1;
    if (strlen(path) + 5 >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    ok = 1;
    if (id && *id) {
        char line[80];
        int n = snprintf(line, sizeof(line), "%s\n", id);
        ok = n > 0 && write(fd, line, (size_t)n) == n;
    }
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static inline int lx06_official_bind_clear_at(const char *path) {
    return lx06_official_bind_write_at(path, "");
}

static inline int lx06_script_has_line(const char *apps, const char *id, const char *line) {
    char path[320];
    char buf[8192];
    ssize_t n;
    int fd;
    size_t ln;
    if (!lx06_bind_name_ok(id) || !line) return 0;
    if (snprintf(path, sizeof(path), "%s/%s", apps, id) >= (int)sizeof(path)) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return 0;
    ln = strlen(line);
    for (ssize_t i = 0; i + (ssize_t)ln <= n; i++) {
        int start = (i == 0) || buf[i - 1] == '\n';
        if (!start || memcmp(buf + i, line, ln)) continue;
        if (i + (ssize_t)ln == n || buf[i + ln] == '\n' || buf[i + ln] == '\r') return 1;
    }
    return 0;
}

static inline int lx06_official_marker(const char *apps, const char *id) {
    return lx06_script_has_line(apps, id, "# LX06-OFFICIALWAKEUP: on");
}

/* Channel text after "# LX06-LOCAL-CHANNEL:". 1 = non-empty, 0 = empty or absent. */
static inline int lx06_local_channel_armed_at(const char *apps, const char *id) {
    char path[320], buf[8192], value[512];
    ssize_t n;
    int fd;
    const char *key = "# LX06-LOCAL-CHANNEL:";
    size_t kn = strlen(key);
    if (!lx06_bind_name_ok(id)) return 0;
    if (snprintf(path, sizeof(path), "%s/%s", apps, id) >= (int)sizeof(path)) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len > kn && !memcmp(p, key, kn)) {
            const char *v = p + kn;
            size_t vn = len - kn;
            while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
            if (!vn || vn >= sizeof(value)) return 0;
            return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static inline int lx06_official_bind_signal_at(const char *path, const char *id) {
    char tmp[256];
    int fd, n;
    char line[80];
    if (!lx06_bind_name_ok(id)) return -1;
    if (strlen(path) + 5 >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    n = snprintf(line, sizeof(line), "%s\n", id);
    if (n < 0 || write(fd, line, (size_t)n) != n) {
        close(fd); unlink(tmp); return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Reads the bind file and the script marker. Clears a stale id.
 * PAUSED keeps the file so the binding returns when scripts are allowed. */
static inline int lx06_official_bind_decide_at(const char *bind_path, const char *apps, char *id, size_t cap) {
    int rc = lx06_official_bind_read_at(bind_path, id, cap);
    if (rc == 0) return LX06_BIND_NONE;
    if (rc < 0) {
        lx06_official_bind_clear_at(bind_path);
        if (id) id[0] = 0;
        return LX06_BIND_CLEARED;
    }
    if (!lx06_scripts_allowed()) return LX06_BIND_PAUSED;
    if (!lx06_official_marker(apps, id)) {
        lx06_official_bind_clear_at(bind_path);
        if (id) id[0] = 0;
        return LX06_BIND_CLEARED;
    }
    return LX06_BIND_ARMED;
}
#endif
