#include "protocol.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

static int append(char *out, size_t capacity, size_t *used, const char *text) {
    size_t length = strlen(text);
    if (length >= capacity - *used) return -1;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return 0;
}

static int append_string(char *out, size_t capacity, size_t *used, const char *text) {
    if (append(out, capacity, used, "\"") != 0) return -1;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        char escaped[7];
        const char *piece = escaped;
        size_t length = 1;
        switch (*p) {
            case '"': piece = "\\\""; length = 2; break;
            case '\\': piece = "\\\\"; length = 2; break;
            case '\b': piece = "\\b"; length = 2; break;
            case '\f': piece = "\\f"; length = 2; break;
            case '\n': piece = "\\n"; length = 2; break;
            case '\r': piece = "\\r"; length = 2; break;
            case '\t': piece = "\\t"; length = 2; break;
            default:
                if (*p < 0x20) {
                    int n = snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    if (n != 6) return -1;
                    length = 6;
                } else {
                    escaped[0] = (char)*p;
                }
                break;
        }
        if (length >= capacity - *used) return -1;
        memcpy(out + *used, piece, length);
        *used += length;
        out[*used] = '\0';
    }
    return append(out, capacity, used, "\"");
}

static int valid_mode(const char *mode) {
    return mode && (strcmp(mode, "auto") == 0 || strcmp(mode, "manual") == 0 ||
                    strcmp(mode, "realtime") == 0);
}

static int valid_state(const char *state) {
    return state && (strcmp(state, "start") == 0 || strcmp(state, "stop") == 0);
}

int xz_protocol_build_hello(char *out, size_t capacity, int aec_enabled) {
    if (!out || capacity == 0) return -1;
    const char *hello = aec_enabled
        ? "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
          "\"features\":{\"aec\":true},"
          "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}"
        : "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
          "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
    size_t length = strlen(hello);
    if (length >= capacity) return -1;
    memcpy(out, hello, length + 1);
    return 0;
}

int xz_protocol_build_listen(char *out, size_t capacity, const char *session_id,
                             const char *state, const char *mode) {
    if (!out || capacity == 0 || !valid_state(state)) return -1;
    if (strcmp(state, "start") == 0 && !valid_mode(mode)) return -1;
    if (strcmp(state, "stop") == 0 && mode && *mode) return -1;

    size_t used = 0;
    out[0] = '\0';
    if (append(out, capacity, &used, "{\"session_id\":") != 0 ||
        append_string(out, capacity, &used, session_id) != 0 ||
        append(out, capacity, &used, ",\"type\":\"listen\",\"state\":") != 0 ||
        append_string(out, capacity, &used, state) != 0) return -1;
    if (strcmp(state, "start") == 0 &&
        (append(out, capacity, &used, ",\"mode\":") != 0 ||
         append_string(out, capacity, &used, mode) != 0)) return -1;
    return append(out, capacity, &used, "}");
}

int xz_protocol_build_abort(char *out, size_t capacity, const char *session_id,
                            const char *reason) {
    if (!out || capacity == 0 || !reason || !*reason) return -1;
    size_t used = 0;
    out[0] = '\0';
    if (append(out, capacity, &used, "{\"session_id\":") != 0 ||
        append_string(out, capacity, &used, session_id) != 0 ||
        append(out, capacity, &used, ",\"type\":\"abort\",\"reason\":") != 0 ||
        append_string(out, capacity, &used, reason) != 0) return -1;
    return append(out, capacity, &used, "}");
}

int xz_protocol_build_goodbye(char *out, size_t capacity, const char *session_id) {
    if (!out || capacity == 0) return -1;
    size_t used = 0;
    out[0] = '\0';
    if (append(out, capacity, &used, "{\"session_id\":") != 0 ||
        append_string(out, capacity, &used, session_id) != 0 ||
        append(out, capacity, &used, ",\"type\":\"goodbye\"}") != 0) return -1;
    return 0;
}

int xz_protocol_build_detect(char *out, size_t capacity, const char *session_id,
                             const char *wake_text) {
    if (!out || capacity == 0 || !wake_text || !*wake_text) return -1;
    size_t used = 0;
    out[0] = '\0';
    if (append(out, capacity, &used, "{\"session_id\":") != 0 ||
        append_string(out, capacity, &used, session_id) != 0 ||
        append(out, capacity, &used, ",\"type\":\"listen\",\"state\":\"detect\",\"text\":") != 0 ||
        append_string(out, capacity, &used, wake_text) != 0) return -1;
    return append(out, capacity, &used, "}");
}

int xz_protocol_parse_server_hello(const char *json, size_t length, xz_hello *hello,
                                   char *session_id, size_t session_id_capacity) {
    if (!json || !hello || !session_id || session_id_capacity == 0) return -1;

    xz_json_doc doc;
    if (xz_json_parse(&doc, json, length) != 0) return -1;

    int rc = -1;
    int version = 1, sample_rate, channels, frame_duration;
    int audio = xz_json_object_get(&doc, doc.root, "audio_params");
    char type[24], transport[24], format[24];
    session_id[0] = '\0';
    if (audio < 0 || doc.tokens[audio].type != XZ_JSON_OBJECT ||
        xz_json_get_string(&doc, doc.root, "type", type, sizeof(type)) != 0 ||
        strcmp(type, "hello") != 0 ||
        xz_json_get_string(&doc, doc.root, "transport", transport, sizeof(transport)) != 0 ||
        strcmp(transport, "websocket") != 0 ||
        xz_json_get_string(&doc, audio, "format", format, sizeof(format)) != 0 ||
        strcmp(format, "opus") != 0 ||
        xz_json_get_int(&doc, audio, "sample_rate", &sample_rate) != 0 ||
        xz_json_get_int(&doc, audio, "channels", &channels) != 0 ||
        xz_json_get_int(&doc, audio, "frame_duration", &frame_duration) != 0) goto done;

    int version_token = xz_json_object_get(&doc, doc.root, "version");
    if (version_token >= 0 && xz_json_get_int(&doc, doc.root, "version", &version) != 0) goto done;
    int session_token = xz_json_object_get(&doc, doc.root, "session_id");
    if (session_token >= 0 &&
        xz_json_get_string(&doc, doc.root, "session_id", session_id, session_id_capacity) != 0)
        goto done;

    hello->version = version;
    hello->sample_rate = sample_rate;
    hello->channels = channels;
    hello->frame_duration_ms = frame_duration;
    hello->transport = "websocket";
    hello->format = "opus";
    hello->session_id = session_id;
    rc = 0;

done:
    xz_json_destroy(&doc);
    return rc;
}
