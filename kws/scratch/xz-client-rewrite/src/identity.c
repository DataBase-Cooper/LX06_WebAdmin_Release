#define _POSIX_C_SOURCE 200809L
#include "identity.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int uuid_is_valid(const char *value) {
    if (!value || strlen(value) != 36) return 0;
    for (size_t index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return 0;
        } else if (!isxdigit((unsigned char)value[index])) {
            return 0;
        }
    }
    return 1;
}

static int read_id(const char *path, char *out, size_t capacity) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    char value[64];
    ssize_t count = read(fd, value, sizeof(value) - 1);
    int saved_errno = errno;
    close(fd);
    if (count < 0) {
        errno = saved_errno;
        return -1;
    }
    value[count] = '\0';
    value[strcspn(value, "\r\n")] = '\0';
    if (!uuid_is_valid(value) || strlen(value) + 1 > capacity) return -1;
    strcpy(out, value);
    return 0;
}

static int random_bytes(unsigned char *out, size_t length) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < length) {
        ssize_t count = read(fd, out + used, length - used);
        if (count <= 0) {
            int saved_errno = errno;
            close(fd);
            errno = saved_errno;
            return -1;
        }
        used += (size_t)count;
    }
    close(fd);
    return 0;
}

static int make_uuid_v4(char *out, size_t capacity) {
    if (capacity < 37) return -1;
    unsigned char bytes[16];
    if (random_bytes(bytes, sizeof(bytes)) != 0) return -1;
    bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80);
    static const char hex[] = "0123456789abcdef";
    size_t output = 0;
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        if (output == 8 || output == 13 || output == 18 || output == 23) out[output++] = '-';
        out[output++] = hex[bytes[index] >> 4];
        out[output++] = hex[bytes[index] & 0x0f];
    }
    out[output] = '\0';
    return 0;
}

int xz_client_id_load_or_create(const char *path, char *out, size_t capacity) {
    if (!path || !*path || !out || capacity < 37) return -1;
    int loaded = read_id(path, out, capacity);
    if (loaded == 0) return 0;
    if (loaded < 0) return -1;

    char generated[37];
    if (make_uuid_v4(generated, sizeof(generated)) != 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno != EEXIST) return -1;
        return read_id(path, out, capacity);
    }
    const char *cursor = generated;
    size_t remaining = strlen(generated);
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) {
            int saved_errno = errno;
            close(fd);
            unlink(path);
            errno = saved_errno;
            return -1;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    if (write(fd, "\n", 1) != 1 || fsync(fd) != 0 || close(fd) != 0) {
        int saved_errno = errno;
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    strcpy(out, generated);
    return 0;
}
