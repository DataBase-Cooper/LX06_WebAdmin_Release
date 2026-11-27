/* LX06OTA1 read-only streaming verifier. No MTD opens, shell or DATA writes.
 * SHA-256 is supplied by the already-vendored LibTomCrypt (Tom St Denis).
 * Integrity is not publisher authentication. Never authorize flashing on SHA alone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "tomcrypt.h"

#define OTA_HEADER 4096
#define OTA_CHUNK 16384
/* Host and ARM use the target libc's legacy stat ABI. */
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
#define OTA_STAT struct stat64
extern int __fxstat64(int, int, struct stat64 *);
#define OTA_FSTAT(fd, st) __fxstat64(3, fd, st)
#else
#define OTA_STAT struct stat
#define OTA_FSTAT(fd, st) fstat(fd, st)
#endif

#include "ota-stream.h"

#ifndef OTA_VERIFY_LIBRARY
static int ota_error(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    return 1;
}
#endif
static int ota_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
static int ota_size(const char *text, size_t limit, size_t *out) {
    size_t n = 0, len = strlen(text);
    if (!len || len > 8 || *text < '1' || *text > '9') return -1;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || n > (limit - (*text - '0')) / 10) return -1;
        n = n * 10 + (*text - '0');
    }
    if (n < 96 || n > limit) return -1;
    *out = n;
    return 0;
}
static int ota_hash_text(const char *v, char *out) {
    if (strlen(v) != 64) return -1;
    for (unsigned i = 0; i < 64; ++i)
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) return -1;
    memcpy(out, v, 65);
    return 0;
}
static int ota_parse(unsigned char *header, ota_manifest *m) {
    static const char *keys[] = {"model", "version", "kernel_size", "kernel_sha256",
        "rootfs_size", "rootfs_sha256", "payload_sha256", "data_schema"};
    size_t end = 0;
    unsigned seen = 0;
    for (; end + 1 < OTA_HEADER; ++end)
        if (header[end] == '\n' && header[end + 1] == '\n') break;
    if (end + 1 >= OTA_HEADER || memcmp(header, "LX06OTA1\n", 9)) return -1;
    for (size_t i = 0; i < end; ++i)
        if (header[i] != '\n' && (header[i] < 32 || header[i] > 126)) return -1;
    for (size_t i = end + 2; i < OTA_HEADER; ++i) if (header[i]) return -1;
    header[end + 1] = 0;
    char *p = (char *)header + 9;
    memset(m, 0, sizeof(*m));
    while (*p) {
        char *nl = strchr(p, '\n'), *eq;
        if (!nl) return -1;
        *nl = 0;
        eq = strchr(p, '=');
        if (!eq || strchr(eq + 1, '=')) return -1;
        *eq++ = 0;
        unsigned k;
        for (k = 0; k < 8 && strcmp(keys[k], p); ++k) {}
        if (k == 8 || (seen & (1u << k))) return -1;
        seen |= 1u << k;
        switch (k) {
        case 0: if (strcmp(eq, "LX06")) return -1; break;
        case 1:
            if (!*eq || strlen(eq) >= sizeof(m->version) || !ota_alnum(*eq)) return -1;
            for (const char *s = eq; *s; ++s)
                if (!ota_alnum(*s) && *s != '.' && *s != '_' && *s != '-') return -1;
            strcpy(m->version, eq); break;
        case 2: if (ota_size(eq, 6u*1024*1024, &m->kernel_size)) return -1; break;
        case 3: if (ota_hash_text(eq, m->kernel_hash)) return -1; break;
        case 4: if (ota_size(eq, 40u*1024*1024, &m->rootfs_size)) return -1; break;
        case 5: if (ota_hash_text(eq, m->rootfs_hash)) return -1; break;
        case 6: if (ota_hash_text(eq, m->payload_hash)) return -1; break;
        case 7: if (strcmp(eq, "1")) return -1; break;
        }
        p = nl + 1;
    }
    return seen == 255 ? 0 : -1;
}
static int ota_hash_matches(hash_state *state, const char *expected) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    char actual[65];
    if (sha256_done(state, digest) != CRYPT_OK) return 0;
    for (unsigned i = 0; i < 32; ++i) {
        actual[i*2] = hex[digest[i] >> 4];
        actual[i*2+1] = hex[digest[i] & 15];
    }
    actual[64] = 0;
    return !strcmp(actual, expected);
}
struct ota_stream {
    unsigned char header[OTA_HEADER], prefix[96];
    size_t used, prefix_used;
    unsigned phase; /* 0 header, 1 kernel, 2 rootfs, 3 complete payload */
    int finished;
    const char *error;
    ota_manifest manifest;
    hash_state image, payload;
};
size_t ota_stream_storage(void) { return sizeof(ota_stream); }
ota_stream *ota_stream_new(void) { return calloc(1, sizeof(ota_stream)); }
void ota_stream_free(ota_stream *s) { if (s) { memset(s, 0, sizeof(*s)); free(s); } }
const char *ota_stream_error(const ota_stream *s) {
    return s ? (s->error ? s->error : "incomplete stream") : "out of memory";
}
static int ota_stream_fail(ota_stream *s, const char *error) {
    if (s && !s->error) s->error = error;
    return -1;
}
static int ota_image_header(const unsigned char *block, size_t length, int kernel) {
    if (kernel) {
        uint64_t needed = 2048;
        if (memcmp(block, "ANDROID!", 8) || memcmp(block + 36, "\0\10\0\0", 4)) return -1;
        for (unsigned offset = 8; offset <= 24; offset += 8) {
            uint64_t section = 0;
            for (int i = 3; i >= 0; --i) section = (section << 8) | block[offset+i];
            if (offset == 8 && !section) return -1;
            needed += (section + 2047) / 2048 * 2048;
        }
        if (needed > length) return -1;
    } else {
        uint64_t used = 0;
        if (memcmp(block, "hsqs", 4) || memcmp(block + 20, "\4\0", 2) ||
            memcmp(block + 28, "\4\0\0\0", 4)) return -1;
        for (int i = 7; i >= 0; --i) used = (used << 8) | block[40+i];
        if (used < 96 || used > length) return -1;
    }
    return 0;
}
int ota_stream_feed(ota_stream *s, const void *data, size_t size) {
    const unsigned char *p = data;
    if (!s || s->error) return -1;
    if (s->finished || (!p && size)) return ota_stream_fail(s, "input after EOF or invalid buffer");
    while (size) {
        size_t n, length;
        if (s->phase == 3) return ota_stream_fail(s, "trailing data");
        length = s->phase == 0 ? OTA_HEADER :
            (s->phase == 1 ? s->manifest.kernel_size : s->manifest.rootfs_size);
        n = length - s->used;
        if (n > size) n = size;
        if (!s->phase) {
            memcpy(s->header + s->used, p, n);
        } else {
            if (s->prefix_used < sizeof(s->prefix)) {
                size_t take = sizeof(s->prefix) - s->prefix_used;
                if (take > n) take = n;
                memcpy(s->prefix + s->prefix_used, p, take);
                s->prefix_used += take;
                if (s->prefix_used == sizeof(s->prefix) &&
                    ota_image_header(s->prefix, length, s->phase == 1))
                    return ota_stream_fail(s, "invalid image header");
            }
            if (sha256_process(&s->image, p, n) != CRYPT_OK ||
                sha256_process(&s->payload, p, n) != CRYPT_OK)
                return ota_stream_fail(s, "SHA-256 failure");
        }
        s->used += n; p += n; size -= n;
        if (s->used != length) continue;
        if (!s->phase) {
            if (ota_parse(s->header, &s->manifest)) return ota_stream_fail(s, "invalid LX06OTA1 header");
            if (sha256_init(&s->payload) != CRYPT_OK) return ota_stream_fail(s, "SHA-256 failure");
        } else {
            const char *expected = s->phase == 1 ? s->manifest.kernel_hash : s->manifest.rootfs_hash;
            if (!ota_hash_matches(&s->image, expected)) return ota_stream_fail(s, "image SHA-256 mismatch");
        }
        ++s->phase; s->used = 0; s->prefix_used = 0;
        if (s->phase < 3 && sha256_init(&s->image) != CRYPT_OK)
            return ota_stream_fail(s, "SHA-256 failure");
    }
    return 0;
}
int ota_stream_finish(ota_stream *s) {
    if (!s || s->error) return -1;
    if (s->finished) return 0;
    if (s->phase != 3) return ota_stream_fail(s, "truncated package");
    if (!ota_hash_matches(&s->payload, s->manifest.payload_hash))
        return ota_stream_fail(s, "payload SHA-256 mismatch");
    s->finished = 1;
    return 0;
}
const ota_manifest *ota_stream_manifest(const ota_stream *s) {
    return s && s->finished && !s->error ? &s->manifest : NULL;
}
#ifndef OTA_VERIFY_LIBRARY
static int ota_verify(FILE *f) {
    unsigned char block[OTA_CHUNK];
    size_t n;
    int rc = 1;
    ota_stream *s = ota_stream_new();
    const ota_manifest *m;
    if (!s) return ota_error("out of memory");
    while ((n = fread(block, 1, sizeof(block), f)) != 0)
        if (ota_stream_feed(s, block, n)) goto done;
    if (ferror(f)) { ota_stream_fail(s, "read failure"); goto done; }
    if (ota_stream_finish(s)) goto done;
    m = ota_stream_manifest(s);
    printf("{\"ok\":true,\"model\":\"LX06\",\"version\":\"%s\",\"kernel_size\":%lu,\"rootfs_size\":%lu,\"authenticated\":false}\n",
           m->version, (unsigned long)m->kernel_size, (unsigned long)m->rootfs_size);
    rc = 0;
done:
    if (rc) ota_error(ota_stream_error(s));
    ota_stream_free(s);
    return rc;
}
/* Walk every directory with O_NOFOLLOW, not just the leaf. */
static int ota_open(const char *path) {
    char *copy = strdup(path), *save = NULL, *part;
    int dir = -1, fd = -1;
    if (!copy) return -1;
    dir = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    part = strtok_r(copy, "/", &save);
    while (dir >= 0 && part) {
        char *next = strtok_r(NULL, "/", &save);
        if (!strcmp(part, "..")) break;
        fd = openat(dir, part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (next ? O_DIRECTORY : 0));
        close(dir); dir = -1;
        if (fd < 0 || !next) break;
        dir = fd; fd = -1; part = next;
    }
    if (dir >= 0) close(dir);
    free(copy);
    return fd;
}
int main(int argc, char **argv) {
    FILE *f = stdin;
    int result;
    if (argc != 3 || strcmp(argv[1], "verify")) return ota_error("usage: ota-verify verify FILE|-");
    if (strcmp(argv[2], "-")) {
        int fd = ota_open(argv[2]);
        OTA_STAT st;
        if (fd < 0) return ota_error("cannot open regular package without symlinks");
        if (OTA_FSTAT(fd, &st) || !S_ISREG(st.st_mode) || !(f = fdopen(fd, "rb"))) {
            close(fd);
            return ota_error("package is not a readable regular file");
        }
    }
    result = ota_verify(f);
    if (f != stdin) fclose(f);
    return result;
}
#endif
