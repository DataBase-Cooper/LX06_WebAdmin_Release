#ifndef XZ_EVENT_H
#define XZ_EVENT_H

#include <stddef.h>

typedef enum {
    XZ_EVENT_UNKNOWN = 0,
    XZ_EVENT_TTS,
    XZ_EVENT_STT,
    XZ_EVENT_LLM,
    XZ_EVENT_MCP,
    XZ_EVENT_ABORT,
    XZ_EVENT_ERROR,
    XZ_EVENT_GOODBYE
} xz_event_type;

typedef struct {
    xz_event_type type;
    char state[32];
    char emotion[32];
    char reason[64];
    char text[512];
    size_t text_bytes;
} xz_server_event;

int xz_event_parse_text(const char *json, size_t length, xz_server_event *event);

#endif
