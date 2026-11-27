#define _POSIX_C_SOURCE 200809L
#include "talk.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XZ_TALK_LIMIT_BYTES 49152 /* 48KB：写到这就转 */
#define XZ_TALK_KEEP_BYTES 16384  /* 保留最近 16KB */

xz_talk *xz_talk_open(const char *path) {
    if (!path || !*path || strcmp(path, "0") == 0 || strcmp(path, "none") == 0)
        return NULL;
    FILE *stream = fopen(path, "a");
    if (!stream) return NULL;
    xz_talk *talk = calloc(1, sizeof(*talk));
    if (!talk) {
        fclose(stream);
        return NULL;
    }
    talk->stream = stream;
    return talk;
}

void xz_talk_close(xz_talk *talk) {
    if (!talk) return;
    if (talk->stream) fclose(talk->stream);
    free(talk);
}

FILE *xz_talk_stream(const xz_talk *talk) {
    return talk ? talk->stream : NULL;
}

void xz_talk_log(xz_talk *talk, const char *role, const char *text) {
    if (!talk || !talk->stream || !text || !*text) return;

    int fd = fileno(talk->stream);
    off_t size = lseek(fd, 0, SEEK_END);
    if (size >= XZ_TALK_LIMIT_BYTES) {
        off_t keep = size > XZ_TALK_KEEP_BYTES ? (off_t)XZ_TALK_KEEP_BYTES : 0;
        (void)ftruncate(fd, keep);
    }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    fprintf(talk->stream, "[%02d:%02d:%02d] %s: ", tmv.tm_hour, tmv.tm_min,
            tmv.tm_sec, role ? role : "?");
    for (const char *p = text; *p; p++) {
        fputc(*p == '\n' || *p == '\r' ? ' ' : *p, talk->stream);
    }
    fputc('\n', talk->stream);
    fflush(talk->stream);
}
