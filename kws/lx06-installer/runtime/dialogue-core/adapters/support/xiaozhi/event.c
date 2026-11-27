#include "event.h"
#include "json.h"

#include <string.h>

static int optional_string(const xz_json_doc *doc, int object, const char *key,
                           char *out, size_t capacity) {
    int token = xz_json_object_get(doc, object, key);
    if (token < 0) {
        out[0] = '\0';
        return 0;
    }
    return xz_json_get_string(doc, object, key, out, capacity);
}

int xz_event_parse_text(const char *json, size_t length, xz_server_event *event) {
    if (!json || !event) return -1;
    memset(event, 0, sizeof(*event));

    xz_json_doc doc;
    if (xz_json_parse(&doc, json, length) != 0) return -1;

    char type[24];
    if (xz_json_get_string(&doc, doc.root, "type", type, sizeof(type)) != 0) {
        xz_json_destroy(&doc);
        return -1;
    }

    if (strcmp(type, "tts") == 0) {
        event->type = XZ_EVENT_TTS;
        if (xz_json_get_string(&doc, doc.root, "state", event->state,
                               sizeof(event->state)) != 0) goto invalid;
    } else if (strcmp(type, "stt") == 0) {
        event->type = XZ_EVENT_STT;
    } else if (strcmp(type, "llm") == 0) {
        event->type = XZ_EVENT_LLM;
        if (optional_string(&doc, doc.root, "emotion", event->emotion,
                            sizeof(event->emotion)) != 0) goto invalid;
    } else if (strcmp(type, "mcp") == 0) {
        event->type = XZ_EVENT_MCP;
    } else if (strcmp(type, "abort") == 0) {
        event->type = XZ_EVENT_ABORT;
        if (optional_string(&doc, doc.root, "reason", event->reason,
                            sizeof(event->reason)) != 0) goto invalid;
    } else if (strcmp(type, "error") == 0) {
        event->type = XZ_EVENT_ERROR;
        if (optional_string(&doc, doc.root, "code", event->reason,
                            sizeof(event->reason)) != 0) goto invalid;
    } else if (strcmp(type, "goodbye") == 0) {
        event->type = XZ_EVENT_GOODBYE;
    } else {
        event->type = XZ_EVENT_UNKNOWN;
    }

    int text_token = xz_json_object_get(&doc, doc.root, "text");
    if (text_token >= 0) {
        if (doc.tokens[text_token].type != XZ_JSON_STRING ||
            xz_json_get_string(&doc, doc.root, "text", event->text,
                               sizeof(event->text)) != 0) goto invalid;
        event->text_bytes = strlen(event->text);
    }
    xz_json_destroy(&doc);
    return 0;

invalid:
    xz_json_destroy(&doc);
    memset(event, 0, sizeof(*event));
    return -1;
}
