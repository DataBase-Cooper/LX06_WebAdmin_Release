#define _POSIX_C_SOURCE 200809L
#include "timeline.h"

#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

void xz_timeline_init(xz_timeline *timeline, FILE *stream) {
    if (!timeline) return;
    timeline->stream = stream ? stream : stderr;
    timeline->sequence = 0;
    timeline->archive_path = NULL;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t xz_timeline_emit_at(xz_timeline *timeline, uint64_t mono_ms,
                             const char *event, const char *details) {
    if (!timeline || !timeline->stream || !event || !*event) return 0;
    /* Bound regular-file traces before the keeper's file-size limit. Pipes
     * and stderr retain normal streaming behavior. No signal-driven rotation
     * is needed under the standard 64 KiB or larger job limit. */
    int fd = fileno(timeline->stream);
    off_t size = lseek(fd, 0, SEEK_END);
    size_t next = 100 + strlen(event) + (details ? strlen(details) : 0);
    struct rlimit limit;
    size_t capacity=49152;
    if(size>=0 && getrlimit(RLIMIT_FSIZE,&limit)==0 && limit.rlim_cur<capacity)
        capacity=(size_t)limit.rlim_cur;
    if (size >= 0 && (size_t)size + next >= capacity) {
        (void)fflush(timeline->stream);
        /* Keep one bounded preceding generation so a poll spanning truncation
         * can still retrieve control events (including PLAYBACK_EXIT). */
        if (timeline->archive_path) {
            FILE *archive = fopen(timeline->archive_path, "w");
            if (archive) {
                char buffer[4096]; off_t offset = 0;
                while (offset < size && offset < 49152) {
                    size_t want = (size_t)(size - offset);
                    if (want > sizeof(buffer)) want = sizeof(buffer);
                    if (want > 49152 - (size_t)offset) want = 49152 - (size_t)offset;
                    ssize_t got = pread(fd, buffer, want, offset);
                    if (got <= 0 || fwrite(buffer, 1, (size_t)got, archive) != (size_t)got) break;
                    offset += got;
                }
                (void)fclose(archive);
            }
        }
        if (ftruncate(fd, 0) == 0) (void)fseek(timeline->stream, 0, SEEK_SET);
    }
    clearerr(timeline->stream);
    uint64_t seq = ++timeline->sequence;
    fprintf(timeline->stream, "[TRACE] seq=%llu mono_ms=%llu event=%s%s%s\n",
            (unsigned long long)seq, (unsigned long long)mono_ms, event,
            details && *details ? " " : "", details && *details ? details : "");
    fflush(timeline->stream);
    return seq;
}

uint64_t xz_timeline_emit(xz_timeline *timeline, const char *event, const char *details) {
    return xz_timeline_emit_at(timeline, monotonic_ms(), event, details);
}
