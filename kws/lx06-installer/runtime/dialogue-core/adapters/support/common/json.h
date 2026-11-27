#ifndef XZ_JSON_H
#define XZ_JSON_H

#include <stddef.h>

typedef enum {
    XZ_JSON_OBJECT = 1,
    XZ_JSON_ARRAY,
    XZ_JSON_STRING,
    XZ_JSON_PRIMITIVE
} xz_json_type;

typedef struct {
    xz_json_type type;
    size_t start;
    size_t end;
    int first_child;
    int last_child;
    int next_sibling;
} xz_json_token;

#ifndef XZ_JSON_MAX_TOKENS
#define XZ_JSON_MAX_TOKENS 1024
#endif

typedef struct {
    const char *source;
    size_t length;
    xz_json_token tokens[XZ_JSON_MAX_TOKENS];
    int token_count;
    int root;
} xz_json_doc;

int xz_json_parse(xz_json_doc *doc, const char *source, size_t length);
void xz_json_destroy(xz_json_doc *doc);
int xz_json_object_get(const xz_json_doc *doc, int object, const char *key);
int xz_json_get_string(const xz_json_doc *doc, int object, const char *key,
                       char *out, size_t capacity);
int xz_json_get_int(const xz_json_doc *doc, int object, const char *key, int *out);

#endif
