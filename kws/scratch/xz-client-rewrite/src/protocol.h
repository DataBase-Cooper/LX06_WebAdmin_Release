#ifndef XZ_PROTOCOL_H
#define XZ_PROTOCOL_H

#include <stddef.h>
#include "session.h"

int xz_protocol_build_hello(char *out, size_t capacity, int aec_enabled, int mcp_enabled);
int xz_protocol_wrap_mcp(char *out, size_t capacity, const char *session_id,
                         const char *payload);
int xz_protocol_build_listen(char *out, size_t capacity, const char *session_id,
                             const char *state, const char *mode);
int xz_protocol_build_abort(char *out, size_t capacity, const char *session_id,
                            const char *reason);
int xz_protocol_build_goodbye(char *out, size_t capacity, const char *session_id);
int xz_protocol_build_detect(char *out, size_t capacity, const char *session_id,
                             const char *wake_text);
int xz_protocol_parse_server_hello(const char *json, size_t length, xz_hello *hello,
                                   char *session_id, size_t session_id_capacity);

#endif
