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
/* Shared internal utilities from this SAME tokenizer/decoder. The caller's
 * token span and the four input hex bytes must already be within its source. */
int xz_json_decode_token(const xz_json_doc *,int,char *,size_t);
int xz_json_hex4(const char *,unsigned *);
int xz_json_object_get(const xz_json_doc *doc, int object, const char *key);
int xz_json_get_string(const xz_json_doc *doc, int object, const char *key,
                       char *out, size_t capacity);
int xz_json_get_int(const xz_json_doc *doc, int object, const char *key, int *out);

#ifdef XZ_JSON_ENABLE_EVENTS
/* Serial, allocation-free view of this SAME tokenizer. No alternate grammar.
 * byte returns [0,255] or -1 on source failure. token/close return 0 to accept,
 * otherwise abort immediately. Container token end equals start until close.
 * Indices, parentage, ordering, depth and token limit match xz_json_parse.
 * Events are provisional until the whole parse returns 0; callers must stage
 * effects. Callbacks may not mutate/reenter the reader/parser. This entry
 * validates syntax only; original caller schema/UTF-8/primitive rules remain.
 * The source owner must bind length and prove stable bytes before publication. */
typedef struct {
    void *context;
    int (*byte)(void *,size_t offset);
    int (*token)(void *,int index,xz_json_type,size_t start,size_t end,int parent);
    int (*close)(void *,int index,size_t end);
} xz_json_events;
int xz_json_visit(const xz_json_events *,size_t length);
/* Reuse the original string decoder for a STRING event's [start,end) span.
 * write receives decoded bytes in order, with no added NUL, and returns 0.
 * strict rejects NUL/lone surrogate escapes as original A EO validation does.
 * Decoded UTF-8/schema/name bounds remain the A owner's responsibility.
 * Output is provisional and must be discarded if the call returns -1. */
int xz_json_decode_visit(const xz_json_events *,size_t start,size_t end,int strict,
    int (*write)(void *,const char *,size_t),void *,size_t *decoded_length);
#endif

#endif
