#ifndef XZ_TIMELINE_H
#define XZ_TIMELINE_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *stream;
    uint64_t sequence;
    const char *archive_path;
} xz_timeline;

void xz_timeline_init(xz_timeline *timeline, FILE *stream);
uint64_t xz_timeline_emit(xz_timeline *timeline, const char *event, const char *details);
uint64_t xz_timeline_emit_at(xz_timeline *timeline, uint64_t mono_ms,
                             const char *event, const char *details);

#endif
