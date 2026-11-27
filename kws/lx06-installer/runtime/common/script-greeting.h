#ifndef LX06_SCRIPT_GREETING_H
#define LX06_SCRIPT_GREETING_H

#ifndef LX06_GREETING_DIR
#define LX06_GREETING_DIR "/data/lx06/greetings"
#endif
#ifndef LX06_GREETING_APPS
#define LX06_GREETING_APPS "/data/lx06/apps"
#endif
#define GREETING_TEXT_BYTES 241
#define GREETING_WAV_MAX (128u * 1024u)
#define GREETING_FILE "greeting.cache"

static inline int greeting_name(const char *name) {
    size_t length = strlen(name);
    return length && length <= 64 && name[0] != '.' &&
        strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == length;
}

static inline int greeting_directory(int parent, const char *name, int create) {
    if (create && mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    GREETING_STAT st;
    if (GREETING_FSTAT(fd, &st) || st.st_uid != 0 || (st.st_mode & 0777) != 0700) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    return fd;
}

static inline int greeting_script_dir(const char *name, int create) {
    if (!greeting_name(name)) { errno = EINVAL; return -1; }
    int parent = greeting_directory(AT_FDCWD, LX06_GREETING_DIR, create);
    if (parent < 0) return -1;
    int fd = greeting_directory(parent, name, create);
    int saved = errno;
    close(parent);
    errno = saved;
    return fd;
}

static inline unsigned greeting_le(const unsigned char *bytes, unsigned n) {
    unsigned value = 0;
    while (n) value = (value << 8) | bytes[--n];
    return value;
}

static inline int greeting_wave(int fd, off_t offset, unsigned *rate, unsigned *channels,
                               unsigned *bytes) {
    GREETING_STAT st;
    unsigned char header[44];
    if (GREETING_FSTAT(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < offset + 44 ||
        st.st_size > offset + GREETING_WAV_MAX ||
        pread(fd, header, sizeof header, offset) != sizeof header) return 0;
    *rate = greeting_le(header + 24, 4);
    *channels = greeting_le(header + 22, 2);
    *bytes = greeting_le(header + 40, 4);
    return !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8) &&
        !memcmp(header + 36, "data", 4) && greeting_le(header + 16, 4) == 16 &&
        greeting_le(header + 20, 2) == 1 && greeting_le(header + 34, 2) == 16 &&
        (*channels == 1 || *channels == 2) &&
        (*rate == 8000 || *rate == 16000 || *rate == 24000 || *rate == 48000) &&
        greeting_le(header + 32, 2) == *channels * 2 &&
        greeting_le(header + 28, 4) == *rate * *channels * 2 &&
        *bytes && !(*bytes % (*channels * 2)) &&
        *bytes == (unsigned)(st.st_size - offset - 44) &&
        greeting_le(header + 4, 4) == *bytes + 36;
}

static inline int greeting_open(const char *name, const char *text, unsigned *rate,
                               unsigned *channels, unsigned *bytes) {
    int dir = greeting_script_dir(name, 0);
    if (dir < 0) return -1;
    int fd = openat(dir, GREETING_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(dir);
    if (fd < 0) return -1;
    GREETING_STAT st;
    char stored[GREETING_TEXT_BYTES];
    size_t length = strnlen(text, sizeof stored);
    if (length == sizeof stored || GREETING_FSTAT(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid || st.st_nlink != 1 || (st.st_mode & 0777) != 0600 ||
        pread(fd, stored, sizeof stored, 0) != sizeof stored ||
        memcmp(stored, text, length + 1) ||
        !greeting_wave(fd, sizeof stored, rate, channels, bytes)) {
        close(fd);
        return -1;
    }
    return fd;
}

#endif
