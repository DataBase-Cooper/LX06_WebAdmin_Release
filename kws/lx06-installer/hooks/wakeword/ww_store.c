#define _GNU_SOURCE
#include "ww_store.h"
#include "wake_policy.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int crash_point(const char *point) {
    const char *want = getenv("WW_STORE_CRASH");
    if (want && point && !strcmp(want, point)) _exit(99);
    return 0;
}
static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
static int name_ok(const char *s) {
    size_t n;
    if (!s || !s[0] || !strcmp(s, ".") || !strcmp(s, "..") || strchr(s, '/')) return 0;
    n = strlen(s);
    return n < 48;
}
static int open_store(const char *path, int *dirfd) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0002)) {
        close(fd); errno = EPERM; return -1;
    }
    *dirfd = fd;
    return 0;
}
static int lock_store(int dirfd) {
    int fd, j;
    if (mkdirat(dirfd, "journal", 0700) && errno != EEXIST) return -1;
    j = openat(dirfd, "journal", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (j < 0) return -1;
    fd = openat(j, "lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(j);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}
static int read_fd(int fd, char **out, size_t *n) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    for (;;) {
        ssize_t r;
        if (len + 1024 > cap) {
            char *grown = realloc(buf, cap * 2);
            if (!grown) { free(buf); return -1; }
            buf = grown; cap *= 2;
            if (cap > 1024 * 1024) { free(buf); errno = EFBIG; return -1; }
        }
        r = read(fd, buf + len, cap - len - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { free(buf); return -1; }
        if (!r) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    *out = buf; *n = len;
    return 0;
}
static int stage_bytes(int dirfd, const char *bytes, size_t n, char *name, size_t namecap) {
    char staged[64];
    int fd, try;
    for (try = 0; try < 8; try++) {
        unsigned r = 0;
        int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (u >= 0) { if (read(u, &r, sizeof r) < 0) r = (unsigned)getpid(); close(u); }
        snprintf(staged, sizeof staged, "journal/stage-%u-%d", r, try);
        fd = openat(dirfd, staged, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
    }
    if (fd < 0) return -1;
    if (write_full(fd, bytes, n) || fsync(fd)) { close(fd); unlinkat(dirfd, staged, 0); return -1; }
    if (close(fd)) { unlinkat(dirfd, staged, 0); return -1; }
    if (strlen(staged) >= namecap) { unlinkat(dirfd, staged, 0); return -1; }
    memcpy(name, staged, strlen(staged) + 1);
    return 0;
}
static int fsync_containing(int dirfd, const char *rel) {
    char parent[96];
    const char *slash = strrchr(rel, '/');
    int fd, rc;
    if (!slash) return fsync(dirfd);
    if ((size_t)(slash - rel) >= sizeof parent) return -1;
    memcpy(parent, rel, (size_t)(slash - rel));
    parent[slash - rel] = 0;
    fd = openat(dirfd, parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    rc = fsync(fd);
    close(fd);
    return rc ? -1 : fsync(dirfd);
}
static int replace_name(int dirfd, const char *staged, const char *dest) {
    struct stat st;
    char prev[80];
    int archived = 0;
    if (fstatat(dirfd, dest, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
            errno = EPERM;
            return -1;
        }
        unsigned r = 0;
        int u = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (u >= 0) { if (read(u, &r, sizeof r) < 0) r = 1; close(u); }
        snprintf(prev, sizeof prev, "journal/prev-%u-%ld", r, (long)getpid());
        /* Keep the live path present until the new file can replace it in one
         * atomic rename. The recovery link must be durable before publication. */
        if (linkat(dirfd, dest, dirfd, prev, 0)) return -1;
        archived = 1;
        if (fsync_containing(dirfd, prev)) return -1;
        crash_point("after-replace-backup");
    } else if (errno != ENOENT) {
        return -1;
    }
    if (renameat(dirfd, staged, dirfd, dest)) return -1;
    if (fsync_containing(dirfd, dest) || fsync_containing(dirfd, staged)) {
        const int saved_errno = errno;
        if (archived) {
            if (renameat(dirfd, prev, dirfd, dest) == 0)
                (void)fsync_containing(dirfd, dest);
        } else if (unlinkat(dirfd, dest, 0) == 0) {
            (void)fsync_containing(dirfd, dest);
        }
        errno = saved_errno;
        return -1;
    }
    /* Publication is complete. A housekeeping failure must not make the
     * caller roll back related data while the new configuration is live. */
    if (archived && unlinkat(dirfd, prev, 0) == 0)
        (void)fsync_containing(dirfd, prev);
    return 0;
}
static int intent_complete(const char *text) {
    return text && strstr(text, "\nEND\n");
}
static int listed(const char *intent, const char *name) {
    char key[64];
    snprintf(key, sizeof key, "\nword=%s\n", name);
    return strstr(intent, key) != NULL;
}
static int wipe_tree(int dirfd, const char *intent) {
    const char *p = strstr(intent, "\nword=");
    int removed = 0;
    while (p) {
        char name[48];
        size_t n = 0;
        p += 6;
        while (p[n] && p[n] != '\n' && n + 1 < sizeof name) { name[n] = p[n]; n++; }
        name[n] = 0;
        if (name_ok(name)) {
            char model[80], rec[80];
            int recfd;
            snprintf(model, sizeof model, "models/%s.lwt", name);
            snprintf(rec, sizeof rec, "recs/%s", name);
            unlinkat(dirfd, model, 0);
            recfd = openat(dirfd, rec, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (recfd >= 0) {
                DIR *dir = fdopendir(recfd);
                if (dir) {
                    struct dirent *ent;
                    while ((ent = readdir(dir))) {
                        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
                        if (strchr(ent->d_name, '/')) continue;
                        unlinkat(recfd, ent->d_name, 0);
                    }
                    closedir(dir);
                } else close(recfd);
                unlinkat(dirfd, rec, AT_REMOVEDIR);
            }
            removed++;
            if (removed == 1) crash_point("after-first-delete");
        }
        p = strstr(p, "\nword=");
    }
    {
        int models = openat(dirfd, "models", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int recs = openat(dirfd, "recs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (models >= 0) { if (fsync(models)) { close(models); if (recs >= 0) close(recs); return -1; } close(models); }
        if (recs >= 0) { if (fsync(recs)) { close(recs); return -1; } close(recs); }
    }
    return fsync(dirfd);
}
static int rewrite_conf(int dirfd, const char *intent) {
    int fd = openat(dirfd, "wakeword.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    char *old = NULL, *out;
    size_t n = 0, cap = 1024, used = 0;
    char notice[2048];
    size_t nn = 0;
    FILE *mem;
    if (fd >= 0) {
        if (read_fd(fd, &old, &n)) { close(fd); return -1; }
        close(fd);
    } else if (errno != ENOENT) return -1;
    out = malloc(cap);
    if (!out) { free(old); return -1; }
    notice[0] = 0;
    if (old) {
        char *save = NULL, *line = strtok_r(old, "\n", &save);
        while (line) {
            int drop = 0;
            if (!strncmp(line, "word=", 5)) {
                char name[48];
                const char *bar = strchr(line, '|');
                size_t ln = bar ? (size_t)(bar - (line + 5)) : strlen(line + 5);
                if (ln < sizeof name) {
                    memcpy(name, line + 5, ln); name[ln] = 0;
                    if (listed(intent, name)) {
                        const char *link = strstr(line, "|link=");
                        drop = 1;
                        if (nn + ln + 80 < sizeof notice) {
                            const char *binding = "-";
                            char bound[68];
                            if (link) {
                                const char *start = link + 6;
                                const char *end = strchr(start, '|');
                                size_t bl = end ? (size_t)(end - start) : strlen(start);
                                if (bl >= sizeof bound) bl = sizeof bound - 1;
                                memcpy(bound, start, bl);
                                bound[bl] = 0;
                                binding = bound;
                            }
                            nn += (size_t)snprintf(notice + nn, sizeof notice - nn, "%s %s\n", name, binding);
                        }
                    }
                }
            }
            if (!drop) {
                size_t ln = strlen(line);
                if (used + ln + 2 > cap) {
                    char *grown = realloc(out, cap * 2);
                    if (!grown) { free(out); free(old); return -1; }
                    out = grown; cap *= 2;
                }
                memcpy(out + used, line, ln); used += ln; out[used++] = '\n';
            }
            line = strtok_r(NULL, "\n", &save);
        }
    }
    out[used] = 0;
    free(old);
    {
        char staged[64];
        if (stage_bytes(dirfd, out, used, staged, sizeof staged) ||
            replace_name(dirfd, staged, "wakeword.conf")) { free(out); return -1; }
    }
    free(out);
    mem = NULL;
    {
        char staged[64];
        if (!stage_bytes(dirfd, notice, nn, staged, sizeof staged) &&
            replace_name(dirfd, staged, "journal/p05-cleared.txt")) { /* notice is best-effort */ }
    }
    (void)mem;
    return 0;
}
int ww_store_migrate(const char *dir, const char *feature) {
    int dirfd = -1, lock = -1, fd = -1, rc = -1;
    char *intent = NULL;
    size_t n = 0;
    struct stat before, after;
    if (!feature || !feature[0] || strchr(feature, '\n') || strlen(feature) > 80) { errno = EINVAL; return -1; }
    if (open_store(dir, &dirfd)) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    if (fstat(dirfd, &before)) goto done;
    lock = lock_store(dirfd);
    if (lock < 0) goto done;
    fd = openat(dirfd, "journal/p05-wipe.done", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        char got[96] = {0};
        ssize_t r = read(fd, got, sizeof got - 1);
        close(fd); fd = -1;
        if (r > 0 && !strncmp(got, feature, strlen(feature)) && got[strlen(feature)] == '\n') { rc = 0; goto done; }
    }
    fd = openat(dirfd, "journal/p05-wipe", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        if (read_fd(fd, &intent, &n)) { close(fd); goto done; }
        close(fd); fd = -1;
        if (!intent_complete(intent) || !strstr(intent, feature)) { free(intent); intent = NULL; }
    }
    if (!intent) {
        int conf = openat(dirfd, "wakeword.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        char *text = NULL;
        size_t tn = 0, cap = 1024, used;
        char *built;
        if (conf >= 0) {
            if (read_fd(conf, &text, &tn)) { close(conf); goto done; }
            close(conf);
        } else if (errno != ENOENT) goto done;
        built = malloc(cap);
        if (!built) { free(text); goto done; }
        used = (size_t)snprintf(built, cap, "P05WIPE1\nfeature=%s\n", feature);
        if (text) {
            char *save = NULL, *line = strtok_r(text, "\n", &save);
            while (line) {
                if (!strncmp(line, "word=", 5)) {
                    const char *bar = strchr(line, '|');
                    char name[48], feat[96] = {0};
                    size_t ln = bar ? (size_t)(bar - (line + 5)) : strlen(line + 5);
                    const char *fp = strstr(line, "|feature=");
                    if (ln < sizeof name) {
                        memcpy(name, line + 5, ln); name[ln] = 0;
                        if (fp) {
                            const char *end = strchr(fp + 9, '|');
                            size_t fl = end ? (size_t)(end - (fp + 9)) : strlen(fp + 9);
                            if (fl < sizeof feat) { memcpy(feat, fp + 9, fl); feat[fl] = 0; }
                        }
                        if (name_ok(name) && strcmp(feat, feature)) {
                            if (used + ln + 8 > cap) {
                                char *grown = realloc(built, cap * 2);
                                if (!grown) { free(built); free(text); goto done; }
                                built = grown; cap *= 2;
                            }
                            used += (size_t)snprintf(built + used, cap - used, "word=%s\n", name);
                        }
                    }
                }
                line = strtok_r(NULL, "\n", &save);
            }
        }
        free(text);
        if (used + 5 < cap) used += (size_t)snprintf(built + used, cap - used, "END\n");
        {
            char staged[64];
            if (stage_bytes(dirfd, built, used, staged, sizeof staged) ||
                replace_name(dirfd, staged, "journal/p05-wipe")) { free(built); goto done; }
        }
        free(built);
        crash_point("after-intent");
        fd = openat(dirfd, "journal/p05-wipe", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || read_fd(fd, &intent, &n)) { if (fd >= 0) close(fd); goto done; }
        close(fd); fd = -1;
    }
    if (!intent_complete(intent)) goto done;
    if (wipe_tree(dirfd, intent) || rewrite_conf(dirfd, intent)) goto done;
    {
        char staged[64], body[96];
        int len = snprintf(body, sizeof body, "%s\n", feature);
        if (stage_bytes(dirfd, body, (size_t)len, staged, sizeof staged) ||
            replace_name(dirfd, staged, "journal/p05-wipe.done")) goto done;
    }
    /* dirfd stays on the inode opened at the start. If the path was replaced,
     * do not report success for the new path. A retry opens whatever is there now. */
    if (fstatat(AT_FDCWD, dir, &after, AT_SYMLINK_NOFOLLOW) ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino) goto done;
    crash_point("after-done");
    rc = 0;
done:
    free(intent);
    if (lock >= 0) close(lock);
    if (dirfd >= 0) close(dirfd);
    return rc;
}
int ww_store_replace_conf(const char *dir, const char *text, size_t n) {
    int dirfd = -1, lock = -1, rc = -1;
    char staged[64];
    struct stat before, after;
    if (!text) return -1;
    if (open_store(dir, &dirfd)) return -1;
    if (fstat(dirfd, &before)) goto done;
    lock = lock_store(dirfd);
    if (lock < 0 || stage_bytes(dirfd, text, n, staged, sizeof staged) ||
        replace_name(dirfd, staged, "wakeword.conf")) goto done;
    if (fstatat(AT_FDCWD, dir, &after, AT_SYMLINK_NOFOLLOW) ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino) goto done;
    rc = 0;
done:
    if (lock >= 0) close(lock);
    if (dirfd >= 0) close(dirfd);
    return rc;
}
static int ww_remove_owned(int parent, const char *name, dev_t device, unsigned depth) {
    struct stat entry;
    if (fstatat(parent, name, &entry, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(entry.st_mode)) return unlinkat(parent, name, 0);
    if (!depth || entry.st_dev != device || entry.st_uid != geteuid() ||
        (entry.st_mode & 0022)) {
        errno = EPERM;
        return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat readback;
    if (fstat(fd, &readback) || readback.st_dev != device || readback.st_ino != entry.st_ino) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(dir);
        if (!item) { if (errno) rc = -1; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (ww_remove_owned(fd, item->d_name, device, depth - 1)) { rc = -1; break; }
    }
    if (!rc && fsync(fd)) rc = -1;
    closedir(dir);
    if (rc) return -1;
    return unlinkat(parent, name, AT_REMOVEDIR);
}

static int ww_remove_group(int root, const char *group, const char *name,
                           dev_t device, unsigned depth) {
    int fd = openat(root, group, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (!rc && (st.st_dev != device || st.st_uid != geteuid() || (st.st_mode & 0022))) {
        errno = EPERM;
        rc = -1;
    }
    if (!rc && (ww_remove_owned(fd, name, device, depth) || fsync(fd))) rc = -1;
    close(fd);
    return rc;
}

/* Old versions saved anonymous previous revisions in the journal. Only
 * recognize the on-disk LWT header or the exact word= configuration line.
 * Keep other words and incomplete migration intents intact. */
static int ww_purge_old_revision(int root, int journal, const char *entry,
                                 const char *word, int notice) {
    int fd = openat(journal, entry, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    char *bytes = NULL;
    size_t n = 0;
    int rc = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) || read_fd(fd, &bytes, &n)) goto done;

    /* Old archive names also covered p05-wipe intents. A complete or torn
     * migration intent is recovery evidence, not a saved configuration. */
    if (!notice && n >= 8 && !memcmp(bytes, "P05WIPE1", 8)) { rc = 0; goto done; }
    if (st.st_nlink != 1) goto done;

    size_t len = strlen(word);
    if (!notice && n >= 16 && !memcmp(bytes, "LWT1", 4)) {
        const unsigned char *header = (const unsigned char *)bytes;
        if (header[12] == len && header[13] == 0 && header[14] == 0 && header[15] == 0 &&
            n >= 16 + len && !memcmp(bytes + 16, word, len)) {
            if (unlinkat(journal, entry, 0)) goto done;
        }
        rc = 0;
        goto done;
    }
    if (memchr(bytes, 0, n)) { rc = 0; goto done; }
    char *from = bytes, *to = bytes, *end = bytes + n;
    size_t prefix = notice ? 0 : 5;
    while (from < end) {
        char *eol = memchr(from, '\n', (size_t)(end - from));
        size_t line = eol ? (size_t)(eol - from) : (size_t)(end - from);
        int match = line >= prefix + len && (!prefix || !memcmp(from, "word=", prefix)) &&
            !memcmp(from + prefix, word, len) &&
            (line == prefix + len || from[prefix + len] == (notice ? ' ' : '|'));
        size_t length = line + (eol != NULL);
        if (!match) { memmove(to, from, length); to += length; }
        from += length;
    }
    if ((size_t)(to - bytes) != n) {
        if (to == bytes) {
            if (unlinkat(journal, entry, 0)) goto done;
        } else {
            char staged[64], path[80];
            if (snprintf(path, sizeof(path), "journal/%s", entry) >= (int)sizeof(path) ||
                stage_bytes(root, bytes, (size_t)(to - bytes), staged, sizeof staged) ||
                replace_name(root, staged, path)) goto done;
        }
    }
    rc = 0;
done:
    free(bytes);
    close(fd);
    return rc;
}

static int ww_purge_word_history(int root, const char *word) {
    int fd = openat(root, "journal", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) rc = -1; break; }
        const char *name = entry->d_name;
        int notice = !strcmp(name, "p05-cleared.txt");
        int archive = !strncmp(name, "prev-", 5) && strlen(name) < 48 &&
            strspn(name + 5, "0123456789-") == strlen(name + 5);
        if ((notice || archive) && ww_purge_old_revision(root, fd, name, word, notice)) {
            rc = -1;
            break;
        }
    }
    if (!rc && fsync(fd)) rc = -1;
    closedir(dir);
    return rc;
}

/* The Web mutation lock excludes recordings/publication during deletion.
 * Follow only the exact word within roots owned by the wake store. */
int ww_store_remove_word(const char *dir, const char *name) {
    if (!name_ok(name)) { errno = EINVAL; return -1; }
    int root, lock, rc = -1, samples = -1;
    struct stat st;
    if (open_store(dir, &root)) return -1;
    lock = lock_store(root);
    if (lock < 0 || fstat(root, &st)) goto done;
    if (ww_remove_group(root, "recs", name, st.st_dev, 2)) goto done;
    static const char *const model_suffixes[] = {
        ".lwt", ".lwt.pending", ".trainfail", ".migration-required"
    };
    for (unsigned i=0; i<sizeof(model_suffixes)/sizeof(model_suffixes[0]); i++) {
        char asset[80];
        snprintf(asset, sizeof asset, "%s%s", name, model_suffixes[i]);
        if (ww_remove_group(root, "models", asset, st.st_dev, 0)) goto done;
    }
    samples = openat(root, "samples", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (samples < 0 && errno != ENOENT) goto done;
    if (samples >= 0) {
        struct stat ss;
        if (fstat(samples, &ss) || ss.st_dev != st.st_dev ||
            ss.st_uid != geteuid() || (ss.st_mode & 0022)) goto done;
        static const char *const purposes[] = {"acceptance", "calibration", "diagnostic"};
        for (unsigned i=0; i<sizeof(purposes)/sizeof(purposes[0]); i++)
            if (ww_remove_group(samples, purposes[i], name, st.st_dev, 2)) goto done;
    }
    if (ww_purge_word_history(root, name)) goto done;
    rc = 0;
done:
    if (samples >= 0) close(samples);
    if (lock >= 0) close(lock);
    close(root);
    return rc;
}

static double ww_gate_value(const char *value, size_t size) {
    char buf[32], *end;
    if (!size || size >= sizeof buf) return 0;
    memcpy(buf, value, size); buf[size] = 0;
    if (strspn(buf, "0123456789.") != size) return 0;
    double gate = strtod(buf, &end);
    return !*end && gate >= .000001 && gate <= WAKE_GATE_CEILING ? gate : 0;
}

/* A retained limit belongs to the last accepted model. Preserve that identity
 * while the current candidate is pending or has failed joint calibration. */
static void ww_accepted_model(const char *line, size_t len, char sha[65]) {
    const char *end=line+len, *current=NULL;
    int verified=1;
    sha[0]=0;
    while(line<end) {
        const char *next=memchr(line,'|',(size_t)(end-line));
        if(!next)next=end;
        size_t n=(size_t)(next-line);
        if(n==77 && !memcmp(line,"model_sha256=",13))current=line+13;
        if(n==81 && !memcmp(line,"p05_accepted_sha=",17) &&
           strspn(line+17,"0123456789abcdef")==64) {
            memcpy(sha,line+17,64);sha[64]=0;
        }
        if((n==13 && !memcmp(line,"p05_pending=1",13)) ||
           (n==14 && !memcmp(line,"p05_conflict=1",14)))verified=0;
        line=next==end?end:next+1;
    }
    if(verified && current && strspn(current,"0123456789abcdef")==64) {
        memcpy(sha,current,64);sha[64]=0;
    }
}

/* Older installs only had a proposed threshold. A completed compatible
 * model without a conflict can seed the accepted gate. Keep the accepted
 * bound if sampling or another word subsequently invalidates this model. */
static double ww_last_accepted_gate(const char *line, size_t len, const char *feature) {
    const size_t gate_key = sizeof(WW_ACCEPTED_GATE_FIELD) - 1;
    const char *end = line + len;
    double threshold = 0, accepted = 0;
    int compatible = 0, bound = 0, pending = 0, conflict = 0;
    while (line < end) {
        const char *next = memchr(line, '|', (size_t)(end - line));
        if (!next) next = end;
        size_t field = (size_t)(next - line);
        if (field > gate_key && !memcmp(line, WW_ACCEPTED_GATE_FIELD, gate_key))
            accepted = ww_gate_value(line + gate_key, field - gate_key);
        else if (field > 10 && !memcmp(line, "threshold=", 10))
            threshold = ww_gate_value(line + 10, field - 10);
        else if (field > 8 && !memcmp(line, "feature=", 8))
            compatible = !feature || (strlen(feature) == field - 8 && !memcmp(line + 8, feature, field - 8));
        else if (field == 77 && !memcmp(line, "model_sha256=", 13))
            bound = strspn(line + 13, "0123456789abcdef") == 64;
        else if (field == 13 && !memcmp(line, "p05_pending=", 12))
            pending = line[12] == '1';
        else if (field == 14 && !memcmp(line, "p05_conflict=", 13))
            conflict = line[13] == '1';
        line = next == end ? end : next + 1;
    }
    if (!compatible) return 0;
    if (bound && !pending && !conflict && threshold > 0 && (!accepted || threshold < accepted))
        accepted = threshold;
    return accepted;
}

/* Quiesce an enrollment before any WAV or reference changes. A failed
 * publication cannot turn the old template into a model for new samples. */
int ww_store_mark_unready(const char *dir, const char *name) {
    int root, lock, fd = -1, rc = -1, found = 0;
    char *conf = NULL, *updated = NULL;
    size_t n = 0, used = 0;
    if (!name_ok(name)) { errno = EINVAL; return -1; }
    if (open_store(dir, &root)) return -1;
    lock = lock_store(root);
    if (lock < 0) goto done;
    fd = openat(root, "wakeword.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || read_fd(fd, &conf, &n) || n > 16000) goto done;
    updated = malloc(n + 128);
    if (!updated) goto done;
    char *line = conf, *end = conf + n;
    size_t label = strlen(name);
    while (line < end) {
        char *stop = memchr(line, '\n', (size_t)(end - line));
        if (!stop) stop = end;
        size_t len = (size_t)(stop - line);
        if (len >= 5 + label && !memcmp(line, "word=", 5) &&
            !memcmp(line + 5, name, label) && (len == 5 + label || line[5 + label] == '|')) {
            if (found++) { errno = EINVAL; goto done; }
            const size_t gate_key = sizeof(WW_ACCEPTED_GATE_FIELD) - 1;
            double accepted = ww_last_accepted_gate(line, len, NULL);
            char accepted_sha[65];
            ww_accepted_model(line,len,accepted_sha);
            int enabled = 1, pending = 0, requested = 0;
            const char *key = line;
            while (key < stop) {
                const char *next = memchr(key, '|', (size_t)(stop - key));
                if (!next) next = stop;
                size_t field = (size_t)(next - key);
                int skip = 0;
                if (field == 9 && !memcmp(key, "enabled=", 8)) { enabled = key[8] == '1'; skip = 1; }
                if (field == 13 && !memcmp(key, "p05_pending=", 12)) { pending = key[12] == '1'; skip = 1; }
                if (field == 15 && !memcmp(key, "p05_requested=", 14)) { requested = key[14] == '1'; skip = 1; }
                if (field >= 13 && !memcmp(key, "model_sha256=", 13)) skip = 1;
                if (field >= gate_key && !memcmp(key, WW_ACCEPTED_GATE_FIELD, gate_key)) skip = 1;
                if ((field>=17 && !memcmp(key,"p05_accepted_sha=",17)) ||
                    (field>=14 && !memcmp(key,"p05_candidate=",14))) skip=1;
                if (!skip) {
                    memcpy(updated + used, key, field);
                    used += field;
                    updated[used++] = '|';
                }
                key = next == stop ? stop : next + 1;
            }
            if (accepted > 0)
                used += (size_t)snprintf(updated + used, n + 128 - used,
                    WW_ACCEPTED_GATE_FIELD "%.6f|", accepted);
            if (accepted>0 && accepted_sha[0])
                used += (size_t)snprintf(updated+used,n+128-used,"p05_accepted_sha=%s|",accepted_sha);
            used += (size_t)snprintf(updated + used, n + 128 - used,
                "enabled=0|p05_pending=1|p05_requested=%d\n", pending ? requested : enabled);
        } else {
            len += stop < end;
            memcpy(updated + used, line, len);
            used += len;
        }
        line = stop < end ? stop + 1 : end;
    }
    if (!found) { errno = ENOENT; goto done; }
    char staged[64];
    if (stage_bytes(root, updated, used, staged, sizeof staged) ||
        replace_name(root, staged, "wakeword.conf")) goto done;
    rc = 0;
done:
    free(updated); free(conf);
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    close(root);
    return rc;
}
static int publish_model_impl(const char *dir, const char *name, const char *model_path,
                           const char *threshold, const char *feature, const char *sha, int candidate) {
    int dirfd = -1, lock = -1, src = -1, rc = -1;
    char *model = NULL, *conf = NULL, *rebuilt = NULL;
    size_t mn = 0, cn = 0, cap, used;
    char staged[64], dest[80];
    struct stat before;
    if (!name_ok(name) || !threshold || !feature || !sha || strlen(sha) != 64) { errno = EINVAL; return -1; }
    if (open_store(dir, &dirfd)) return -1;
    if (fstat(dirfd, &before)) goto done;
    lock = lock_store(dirfd);
    if (lock < 0) goto done;
    src = open(model_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (src < 0 || read_fd(src, &model, &mn)) goto done;
    close(src); src = -1;
    if (mkdirat(dirfd, "models", 0755) && errno != EEXIST) goto done;
    snprintf(dest, sizeof dest, "models/%s.lwt", name);
    if (stage_bytes(dirfd, model, mn, staged, sizeof staged) || replace_name(dirfd, staged, dest)) goto done;
    src = openat(dirfd, "wakeword.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (src >= 0) {
        if (read_fd(src, &conf, &cn)) goto done;
        close(src); src = -1;
    } else if (errno != ENOENT) goto done;
    cap = (conf ? cn : 0) + 256; used = 0;
    rebuilt = malloc(cap);
    if (!rebuilt) goto done;
    if (conf) {
        char *save = NULL, *line = strtok_r(conf, "\n", &save);
        int found = 0;
        while (line) {
            char piece[768];
            int keep = 1;
            if (!strncmp(line, "word=", 5)) {
                const char *bar = strchr(line + 5, '|');
                size_t ln = bar ? (size_t)(bar - (line + 5)) : strlen(line + 5);
                if (ln == strlen(name) && !memcmp(line + 5, name, ln)) {
                    char extra[256] = {0};
                    const size_t gate_key = sizeof(WW_ACCEPTED_GATE_FIELD) - 1;
                    double accepted = candidate ? ww_last_accepted_gate(line, strlen(line), feature) : 0;
                    char accepted_sha[65];
                    ww_accepted_model(line,strlen(line),accepted_sha);
                    size_t eu = 0; int enabled = 1, pending = 0, requested = 0;
                    const char *field = bar ? bar + 1 : "";
                    while (*field) {
                        const char *next = strchr(field, '|');
                        size_t fl = next ? (size_t)(next - field) : strlen(field);
                        if (fl == 9 && !memcmp(field, "enabled=", 8)) enabled = field[8] == '1';
                        if (fl == 13 && !memcmp(field, "p05_pending=", 12)) pending = field[12] == '1';
                        if (fl == 15 && !memcmp(field, "p05_requested=", 14)) requested = field[14] == '1';
                        if (strncmp(field,"p05_accepted_sha=",17) && strncmp(field,"p05_candidate=",14) &&
                            (!candidate || (strncmp(field, "enabled=", 8) && strncmp(field, "p05_pending=", 12) &&
                            strncmp(field, "p05_requested=", 14) && (fl < gate_key || memcmp(field, WW_ACCEPTED_GATE_FIELD, gate_key)))) &&
                            (strncmp(field, "threshold=", 10) && strncmp(field, "feature=", 8) &&
                             strncmp(field, "model_sha256=", 13))) {
                            if (eu + fl + 2 < sizeof extra) {
                                if (eu) extra[eu++] = '|';
                                memcpy(extra + eu, field, fl); eu += fl; extra[eu] = 0;
                            }
                        }
                        if (!next) break;
                        field = next + 1;
                    }
                    char limited[32];
                    const char *gate = threshold;
                    if (accepted > 0 && accepted < ww_gate_value(threshold, strlen(threshold))) {
                        snprintf(limited, sizeof limited, "%.6f", accepted);
                        gate = limited;
                    }
                    snprintf(piece, sizeof piece, "word=%s|threshold=%s|feature=%s|model_sha256=%s%s%s",
                             name, gate, feature, sha, eu ? "|" : "", extra);
                    if (candidate) {
                        size_t pl = strlen(piece);
                        int n = snprintf(piece + pl, sizeof(piece)-pl,
                            "|enabled=0|p05_pending=1|p05_requested=%d", pending ? requested : enabled);
                        if (n < 0 || (size_t)n >= sizeof(piece)-pl) { errno = EFBIG; goto done; }
                        pl += (size_t)n;
                        if (accepted > 0) {
                            n = snprintf(piece + pl, sizeof(piece)-pl, "|" WW_ACCEPTED_GATE_FIELD "%.6f", accepted);
                            if (n < 0 || (size_t)n >= sizeof(piece)-pl) { errno = EFBIG; goto done; }
                            pl+=(size_t)n;
                            if(accepted_sha[0]) {
                                n=snprintf(piece+pl,sizeof(piece)-pl,"|p05_accepted_sha=%s",accepted_sha);
                                if(n<0 || (size_t)n>=sizeof(piece)-pl){errno=EFBIG;goto done;}
                                pl+=(size_t)n;
                                if(strcmp(accepted_sha,sha) && gate==limited) {
                                    n=snprintf(piece+pl,sizeof(piece)-pl,"|p05_candidate=%s",threshold);
                                    if(n<0 || (size_t)n>=sizeof(piece)-pl){errno=EFBIG;goto done;}
                                }
                            }
                        }
                    }
                    found = 1; keep = 0;
                    if (used + strlen(piece) + 2 > cap) { errno = EFBIG; goto done; }
                    used += (size_t)sprintf(rebuilt + used, "%s\n", piece);
                }
            }
            if (keep) {
                if (used + strlen(line) + 2 > cap) { errno = EFBIG; goto done; }
                used += (size_t)sprintf(rebuilt + used, "%s\n", line);
            }
            line = strtok_r(NULL, "\n", &save);
        }
        if (!found) { errno = ENOENT; goto done; }
    } else { errno = ENOENT; goto done; }
    if (stage_bytes(dirfd, rebuilt, used, staged, sizeof staged) ||
        replace_name(dirfd, staged, "wakeword.conf")) goto done;
    {
        struct stat after;
        if (fstatat(AT_FDCWD, dir, &after, AT_SYMLINK_NOFOLLOW) ||
            after.st_dev != before.st_dev || after.st_ino != before.st_ino) goto done;
    }
    rc = 0;
done:
    free(model); free(conf); free(rebuilt);
    if (src >= 0) close(src);
    if (lock >= 0) close(lock);
    if (dirfd >= 0) close(dirfd);
    return rc;
}
int ww_store_publish_model(const char *dir, const char *name, const char *model_path,
                           const char *threshold, const char *feature, const char *sha) {
    return publish_model_impl(dir,name,model_path,threshold,feature,sha,0);
}
#ifdef WW_STORE_TOOL
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "mark-unready")) return ww_store_mark_unready(argv[2], argv[3]) ? 1 : 0;
    if (argc == 4 && !strcmp(argv[1], "migrate")) return ww_store_migrate(argv[2], argv[3]) ? 1 : 0;
    if (argc == 8 && !strcmp(argv[1], "publish-candidate"))
        return publish_model_impl(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7],1) ? 1 : 0;
    if (argc == 8 && !strcmp(argv[1], "publish-model"))
        return ww_store_publish_model(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]) ? 1 : 0;
    if (argc == 3 && !strcmp(argv[1], "replace-conf")) {
        char *text = NULL; size_t n = 0; int fd = open(argv[2], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        /* replace-conf is not used by the CLI; publish-model updates one word. */
        (void)text; (void)n; (void)fd;
    }
    fprintf(stderr, "usage: ww_store migrate DIR FEATURE | publish-model DIR NAME FILE THRESHOLD FEATURE SHA\n");
    return 2;
}
#endif
