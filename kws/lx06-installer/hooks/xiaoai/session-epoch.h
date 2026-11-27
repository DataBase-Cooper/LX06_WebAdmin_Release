#ifndef LX06_SESSION_EPOCH_H
#define LX06_SESSION_EPOCH_H

#include "session-owner.h"
#include <sys/syscall.h>
#include <time.h>

#define LX06_IDLE_EPOCH_BYTES 16
#define LX06_IDLE_EPOCH_HEX (2 * LX06_IDLE_EPOCH_BYTES)
#ifndef LX06_IDLE_EPOCH_FILE
#define LX06_IDLE_EPOCH_FILE LX06_SESSION_FILE ".epoch"
#endif
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __fxstat64(int, int, struct stat64 *);
#define LX06_EPOCH_FSTAT(fd, st) __fxstat64(3, (fd), (st))
#else
#define LX06_EPOCH_FSTAT(fd, st) fstat((fd), (st))
#endif

static inline int lx06_idle_epoch_read(unsigned char out[LX06_IDLE_EPOCH_BYTES]) {
    int fd = open(LX06_IDLE_EPOCH_FILE, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return -1;
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
    struct stat64 st;
#else
    struct stat st;
#endif
    unsigned char data[LX06_IDLE_EPOCH_BYTES + 1];
    ssize_t n = -1;
    if (LX06_EPOCH_FSTAT(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        st.st_uid == geteuid() && !(st.st_mode & 0077)) {
        do { n = read(fd, data, sizeof(data)); } while (n < 0 && errno == EINTR);
    }
    close(fd);
    static const unsigned char zero[LX06_IDLE_EPOCH_BYTES];
    if (n != LX06_IDLE_EPOCH_BYTES || !memcmp(data, zero, sizeof(zero))) {
        errno = EINVAL;
        return -1;
    }
    memcpy(out, data, LX06_IDLE_EPOCH_BYTES);
    return 0;
}

/* Call only while holding the exclusive session lease, before admitting an
 * action that can use speaker audio. Keep the lease inode and metadata ABI. */
static inline int lx06_idle_epoch_changed(int lease) {
    if (lease < 0) { errno = EINVAL; return -1; }
    unsigned char value[LX06_IDLE_EPOCH_BYTES];
    static const unsigned char zero[LX06_IDLE_EPOCH_BYTES];
#ifdef SYS_getrandom
    ssize_t n;
    do { n = syscall(SYS_getrandom, value, sizeof(value), 1u); } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(value) || !memcmp(value, zero, sizeof(value))) { errno = EIO; return -1; }
#else
    errno = ENOSYS;
    return -1;
#endif
    char temporary[640];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", LX06_IDLE_EPOCH_FILE);
    if (length < 0 || (size_t)length >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t written;
    do { written = write(fd, value, sizeof(value)); } while (written < 0 && errno == EINTR);
    int saved = written == (ssize_t)sizeof(value) ? 0 : EIO;
    if (close(fd) && !saved) saved = errno;
    if (!saved && rename(temporary, LX06_IDLE_EPOCH_FILE) == 0) return 0;
    if (!saved) saved = errno;
    unlink(temporary);
    errno = saved;
    return -1;
}

static inline int lx06_idle_epoch_snapshot(unsigned char out[LX06_IDLE_EPOCH_BYTES]) {
    int lease = lx06_session_lock_idle(LOCK_SH);
    if (lease < 0) return -1;
    int result = lx06_idle_epoch_read(out);
    if (result < 0 && errno == ENOENT) {
        close(lease);
        lease = lx06_session_take_idle();
        if (lease < 0) return -1;
        result = lx06_idle_epoch_read(out);
        if (result < 0 && errno == ENOENT && lx06_idle_epoch_changed(lease) == 0)
            result = lx06_idle_epoch_read(out);
    }
    int saved = errno;
    close(lease);
    errno = saved;
    return result;
}

static inline int lx06_idle_epoch_lock(const unsigned char expected[LX06_IDLE_EPOCH_BYTES],int operation) {
    int lease = lx06_session_lock_idle(operation);
    if (lease < 0) return -1;
    unsigned char current[LX06_IDLE_EPOCH_BYTES];
    if (lx06_idle_epoch_read(current) == 0 && !memcmp(current, expected, sizeof(current)))
        return lease;
    close(lease);
    errno = ESTALE;
    return -1;
}
/* Hold against real audio admission while permitting capture observers. */
static inline int lx06_idle_epoch_observe(const unsigned char expected[LX06_IDLE_EPOCH_BYTES]) {
    return lx06_idle_epoch_lock(expected,LOCK_SH);
}
static inline int lx06_idle_epoch_take(const unsigned char expected[LX06_IDLE_EPOCH_BYTES]) {
    return lx06_idle_epoch_lock(expected,LOCK_EX);
}

static inline int lx06_idle_epoch_parse(const char *hex, unsigned char out[LX06_IDLE_EPOCH_BYTES]) {
    static const char digits[] = "0123456789abcdef";
    static const unsigned char zero[LX06_IDLE_EPOCH_BYTES];
    if (strlen(hex) != LX06_IDLE_EPOCH_HEX) return -1;
    for (size_t i = 0; i < LX06_IDLE_EPOCH_BYTES; ++i) {
        const char *high = strchr(digits, hex[i * 2]);
        const char *low = strchr(digits, hex[i * 2 + 1]);
        if (!high || !low) return -1;
        out[i] = (unsigned char)(((high - digits) << 4) | (low - digits));
    }
    return !memcmp(out, zero, sizeof(zero)) ? -1 : 0;
}

static inline uint64_t lx06_idle_epoch_now_ms(void) {
    /* Existing 32-bit OEM kernel uses the original timespec syscall ABI. */
    struct { long sec, nsec; } time;
    if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &time) || time.sec < 0 || time.nsec < 0)
        return 0;
    return (uint64_t)time.sec * 1000 + (unsigned long)time.nsec / 1000000;
}

#endif
