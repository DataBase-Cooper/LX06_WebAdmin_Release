#ifndef LX06_BOUNDED_LOG_H
#define LX06_BOUNDED_LOG_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* The caller supplies a trusted directory (or a fixed, trusted path) and an
 * OEM-compatible fstat. Never rotate by renaming the current writer's inode. */
#ifndef LX06_LOG_FSTAT
#define LX06_LOG_FSTAT fstat
#endif
#ifndef LX06_LOG_STAT
#define LX06_LOG_STAT struct stat
#endif
#define LX06_LOG_LIMIT 65536

static inline int lx06_log_regular(int fd)
{
    LX06_LOG_STAT st;
    return LX06_LOG_FSTAT(fd, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && st.st_nlink == 1 && !(st.st_mode & 0022);
}

static inline int lx06_log_write(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t wrote = write(fd, data, len);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        data += wrote;
        len -= (size_t)wrote;
    }
    return 0;
}

static inline void lx06_log_previous(int dir, const char *name, int current, off_t end)
{
    char previous[256], chunk[4096];
    int n = snprintf(previous, sizeof previous, "%s.1", name);
    if (n < 0 || (size_t)n >= sizeof previous) return;
    int fd = openat(dir, previous, O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    if (lx06_log_regular(fd) && ftruncate(fd, 0) == 0) {
        off_t off = end > LX06_LOG_LIMIT ? end - LX06_LOG_LIMIT : 0;
        while (off < end) {
            size_t len = end - off > (off_t)sizeof chunk ? sizeof chunk : (size_t)(end - off);
            ssize_t got = pread(current, chunk, len, off);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0 || lx06_log_write(fd, chunk, (size_t)got)) break;
            off += got;
        }
    }
    close(fd);
}

static inline int lx06_log_append_fd(int dir, const char *name, int fd,
                                     const char *data, size_t len)
{
    off_t end;
    int rc = -1;
    if (len > LX06_LOG_LIMIT) {
        data += len - LX06_LOG_LIMIT;
        len = LX06_LOG_LIMIT;
    }
    while (flock(fd, LOCK_EX) < 0) if (errno != EINTR) return -1;
    end = lseek(fd, 0, SEEK_END);
    if (end >= 0) {
        if (end > (off_t)(LX06_LOG_LIMIT - len)) {
            lx06_log_previous(dir, name, fd, end);
            if (ftruncate(fd, 0) || lseek(fd, 0, SEEK_SET) < 0) goto done;
        }
        rc = lx06_log_write(fd, data, len);
    }
done:
    (void)flock(fd, LOCK_UN);
    return rc;
}

static inline int lx06_log_append(int dir, const char *name, const char *data, size_t len)
{
    int rc = -1;
    int fd = openat(dir, name, O_RDWR | O_APPEND | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (lx06_log_regular(fd)) rc = lx06_log_append_fd(dir, name, fd, data, len);
    close(fd);
    return rc;
}
#endif
