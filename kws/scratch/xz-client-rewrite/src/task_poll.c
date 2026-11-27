#include "task_poll.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define XZ_TASK_MAX_AGE_SECONDS 90

int xz_task_session_open(const char *root, char path[XZ_TASK_PATH_MAX]) {
    struct stat info;
    char dir[XZ_TASK_PATH_MAX - 8], link[XZ_TASK_PATH_MAX], active[XZ_TASK_PATH_MAX];
    /* Leave room for the lease name and all filenames used beneath it. */
    if (strlen(root) >= sizeof(dir) - 32) return -1;
    (void)mkdir(root, 0700);
    if (lstat(root, &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 077)) return -1;

    snprintf(dir, sizeof(dir), "%s/session.XXXXXX", root);
    if (!mkdtemp(dir)) return -1;
    snprintf(link, sizeof(link), "%s/lease", dir);
    snprintf(active, sizeof(active), "%s/active", root);
    if (symlink(strrchr(dir, '/') + 1, link) || rename(link, active)) {
        unlink(link);
        rmdir(dir);
        return -1;
    }
    memcpy(path, dir, strlen(dir) + 1);
    return 0;
}

void xz_task_session_close(char path[XZ_TASK_PATH_MAX]) {
    if (!*path) return;
    char closed[XZ_TASK_PATH_MAX - 8];
    int len = snprintf(closed, sizeof(closed), "%s.closed", path);
    if (len < 0 || (size_t)len >= sizeof(closed)) return;
    /* Retire the directory atomically against a concurrent status update. */
    if (rename(path, closed)) return;
    DIR *dir = opendir(closed);
    if (dir) {
        struct dirent *item;
        while ((item = readdir(dir))) {
            if (item->d_name[0] != '.') unlinkat(dirfd(dir), item->d_name, 0);
        }
        closedir(dir);
    }
    rmdir(closed);
}

static int task_id_char(char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '_' || value == '-';
}

static xz_task_phase status_read(int directory, const char *name, xz_task *task) {
    size_t name_len = strlen(name);
    if (name_len < 7 || name_len > XZ_TASK_ID_MAX + 6 || strcmp(name + name_len - 6, ".state"))
        return XZ_TASK_NONE;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return XZ_TASK_NONE;
    struct stat info;
    time_t now = time(NULL);
    char buf[XZ_TASK_ID_MAX + XZ_TASK_QUERY_MAX + 32];
    ssize_t bytes = -1;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
        !(info.st_mode & 022) && info.st_size > 0 && info.st_size < (off_t)sizeof(buf) &&
        now != (time_t)-1 && difftime(now, info.st_mtime) >= -5 &&
        difftime(now, info.st_mtime) <= XZ_TASK_MAX_AGE_SECONDS)
        bytes = read(fd, buf, (size_t)info.st_size);
    close(fd);
    if (bytes <= 4 || bytes != info.st_size || buf[bytes - 1] != '\n' ||
        memchr(buf, 0, (size_t)bytes)) return XZ_TASK_NONE;
    int has_query = !memcmp(buf, "v2 ", 3);
    if (!has_query && memcmp(buf, "v1 ", 3)) return XZ_TASK_NONE;
    buf[--bytes] = 0;
    task->query[0] = 0;
    if (has_query) {
        char *line = strchr(buf + 3, '\n');
        if (!line) return XZ_TASK_NONE;
        *line++ = 0;
        size_t length = strlen(line);
        if (!length || length > XZ_TASK_QUERY_MAX) return XZ_TASK_NONE;
        for (size_t i = 0; i < length; ++i) {
            unsigned char ch = (unsigned char)line[i];
            if (ch < 32 || ch == 127) return XZ_TASK_NONE;
        }
        memcpy(task->query, line, length + 1);
    }
    char *status = strchr(buf + 3, ' ');
    if (!status) return XZ_TASK_NONE;
    *status++ = 0;
    size_t length = strlen(buf + 3);
    if (length != name_len - 6 || memcmp(buf + 3, name, length)) return XZ_TASK_NONE;
    for (size_t i = 0; i < length; ++i)
        if (!task_id_char(buf[3 + i])) return XZ_TASK_NONE;
    memcpy(task->id, buf + 3, length + 1);
    if (!strcmp(status, "running")) return XZ_TASK_RUNNING;
    if (!strcmp(status, "ready")) return XZ_TASK_READY;
    if (!strcmp(status, "queried")) return XZ_TASK_QUERIED;
    return XZ_TASK_NONE;
}

void xz_task_poll_refresh(xz_task_poll *poll, const char *path, uint64_t now) {
    if (now < poll->refresh_at) return;
    poll->refresh_at = now + 1000;
    xz_task tasks[XZ_TASK_MAX_PENDING];
    size_t count = 0;
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *item;
        while (count < XZ_TASK_MAX_PENDING && (item = readdir(dir))) {
            xz_task task = {0};
            task.phase = status_read(dirfd(dir), item->d_name, &task);
            if (task.phase != XZ_TASK_RUNNING && task.phase != XZ_TASK_READY) continue;
            task.query_at = task.phase == XZ_TASK_READY ? now : now + XZ_TASK_POLL_MS;
            for (size_t i = 0; i < poll->count; ++i) {
                const xz_task *prev = &poll->tasks[i];
                if (strcmp(prev->id, task.id)) continue;
                if (prev->phase == task.phase) task.query_at = prev->query_at;
                break;
            }
            tasks[count++] = task;
        }
        closedir(dir);
    }
    memcpy(poll->tasks, tasks, count * sizeof(*tasks));
    poll->count = count;
}

xz_task *xz_task_poll_next(xz_task_poll *poll, const char *fallback, uint64_t now, int *active) {
    xz_task *next = NULL;
    *active = poll->awaiting_until != 0;
    for (size_t i = 0; i < poll->count; ++i) {
        xz_task *task = &poll->tasks[i];
        if (!*task->query && !*fallback) continue;
        *active = 1;
        if (poll->awaiting_until || now < task->query_at) continue;
        if (!next || task->phase > next->phase ||
            (task->phase == next->phase && task->query_at < next->query_at)) next = task;
    }
    return next;
}

int xz_task_query_text(char *out, size_t cap, const char *pattern, const char *id) {
    if (!*pattern || strlen(pattern) > XZ_TASK_QUERY_MAX) return -1;
    const char *slot = strstr(pattern, "{id}");
    int n = slot ? snprintf(out, cap, "%.*s%s%s", (int)(slot-pattern), pattern, id, slot+4)
                 : snprintf(out, cap, "%s", pattern);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
