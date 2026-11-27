#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    xz_json_doc *doc;
    size_t position;
} parser;

static void skip_space(parser *p) {
    while (p->position < p->doc->length &&
           isspace((unsigned char)p->doc->source[p->position])) p->position++;
}

static int add_token(parser *p, xz_json_type type, size_t start, size_t end, int parent) {
    if (p->doc->token_count >= XZ_JSON_MAX_TOKENS) return -1;
    int index = p->doc->token_count++;
    xz_json_token *t = &p->doc->tokens[index];
    t->type = type;
    t->start = start;
    t->end = end;
    t->first_child = -1;
    t->last_child = -1;
    t->next_sibling = -1;
    if (parent >= 0) {
        xz_json_token *pt = &p->doc->tokens[parent];
        if (pt->first_child < 0) pt->first_child = index;
        else p->doc->tokens[pt->last_child].next_sibling = index;
        pt->last_child = index;
    }
    return index;
}

static int parse_string(parser *p, int parent) {
    if (p->position >= p->doc->length || p->doc->source[p->position] != '"') return -1;
    p->position++;
    size_t start = p->position;
    while (p->position < p->doc->length) {
        unsigned char c = (unsigned char)p->doc->source[p->position];
        if (c == '"') {
            size_t end = p->position++;
            return add_token(p, XZ_JSON_STRING, start, end, parent);
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            p->position++;
            if (p->position >= p->doc->length) return -1;
            char e = p->doc->source[p->position++];
            if (e == 'u') {
                if (p->position + 4 > p->doc->length) return -1;
                for (int i = 0; i < 4; ++i)
                    if (!isxdigit((unsigned char)p->doc->source[p->position + (size_t)i])) return -1;
                p->position += 4;
            } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' &&
                       e != 'n' && e != 'r' && e != 't') return -1;
        } else {
            p->position++;
        }
    }
    return -1;
}

static int parse_value(parser *p, int parent, unsigned depth);

static int parse_container(parser *p, int parent, unsigned depth, char open, char close,
                           xz_json_type type) {
    size_t start = p->position++;
    int index = add_token(p, type, start, start, parent);
    if (index < 0) return -1;
    skip_space(p);
    if (p->position < p->doc->length && p->doc->source[p->position] == close) {
        p->doc->tokens[index].end = ++p->position;
        return index;
    }
    for (;;) {
        skip_space(p);
        if (type == XZ_JSON_OBJECT) {
            if (parse_string(p, index) < 0) return -1;
            skip_space(p);
            if (p->position >= p->doc->length || p->doc->source[p->position++] != ':') return -1;
            skip_space(p);
            if (parse_value(p, index, depth + 1) < 0) return -1;
        } else {
            if (parse_value(p, index, depth + 1) < 0) return -1;
        }
        skip_space(p);
        if (p->position >= p->doc->length) return -1;
        char c = p->doc->source[p->position++];
        if (c == close) {
            p->doc->tokens[index].end = p->position;
            return index;
        }
        if (c != ',') return -1;
    }
    (void)open;
}

static int parse_value(parser *p, int parent, unsigned depth) {
    if (depth > 64) return -1;
    skip_space(p);
    if (p->position >= p->doc->length) return -1;
    char c = p->doc->source[p->position];
    if (c == '"') return parse_string(p, parent);
    if (c == '{') return parse_container(p, parent, depth, '{', '}', XZ_JSON_OBJECT);
    if (c == '[') return parse_container(p, parent, depth, '[', ']', XZ_JSON_ARRAY);
    size_t start = p->position;
    while (p->position < p->doc->length) {
        c = p->doc->source[p->position];
        if (c == ',' || c == ']' || c == '}' || isspace((unsigned char)c)) break;
        p->position++;
    }
    if (p->position == start) return -1;
    return add_token(p, XZ_JSON_PRIMITIVE, start, p->position, parent);
}

int xz_json_parse(xz_json_doc *doc, const char *source, size_t length) {
    if (!doc || !source || length == 0) return -1;
    memset(doc, 0, sizeof(*doc));
    doc->source = source;
    doc->length = length;
    doc->root = -1;
    parser p = {doc, 0};
    doc->root = parse_value(&p, -1, 0);
    if (doc->root < 0) return -1;
    skip_space(&p);
    return p.position == length && doc->tokens[doc->root].type == XZ_JSON_OBJECT ? 0 : -1;
}

void xz_json_destroy(xz_json_doc *doc) {
    if (doc) memset(doc, 0, sizeof(*doc));
}

static int token_equals(const xz_json_doc *doc, int token, const char *text) {
    if (!doc || token < 0 || token >= doc->token_count || !text) return 0;
    const xz_json_token *t = &doc->tokens[token];
    size_t n = strlen(text);
    return t->type == XZ_JSON_STRING && t->end - t->start == n &&
           memcmp(doc->source + t->start, text, n) == 0;
}

int xz_json_object_get(const xz_json_doc *doc, int object, const char *key) {
    if (!doc || object < 0 || object >= doc->token_count || !key ||
        doc->tokens[object].type != XZ_JSON_OBJECT) return -1;
    int k = doc->tokens[object].first_child;
    while (k >= 0) {
        int value = doc->tokens[k].next_sibling;
        if (value < 0) return -1;
        if (token_equals(doc, k, key)) return value;
        k = doc->tokens[value].next_sibling;
    }
    return -1;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    *out = v;
    return 0;
}

static int append_utf8(char *out, size_t cap, size_t *used, unsigned cp) {
    unsigned char b[4];
    size_t n;
    if (cp <= 0x7f) { b[0] = (unsigned char)cp; n = 1; }
    else if (cp <= 0x7ff) {
        b[0] = (unsigned char)(0xc0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3f)); n = 2;
    } else if (cp <= 0xffff) {
        b[0] = (unsigned char)(0xe0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        b[2] = (unsigned char)(0x80 | (cp & 0x3f)); n = 3;
    } else {
        b[0] = (unsigned char)(0xf0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        b[3] = (unsigned char)(0x80 | (cp & 0x3f)); n = 4;
    }
    if (*used + n >= cap) return -1;
    memcpy(out + *used, b, n);
    *used += n;
    return 0;
}

static int decode_string(const xz_json_doc *doc, int token, char *out, size_t cap) {
    if (!doc || token < 0 || token >= doc->token_count || !out || cap == 0 ||
        doc->tokens[token].type != XZ_JSON_STRING) return -1;
    const xz_json_token *t = &doc->tokens[token];
    size_t used = 0;
    for (size_t i = t->start; i < t->end; ++i) {
        unsigned char c = (unsigned char)doc->source[i];
        if (c != '\\') {
            if (used + 1 >= cap) return -1;
            out[used++] = (char)c;
            continue;
        }
        if (++i >= t->end) return -1;
        char e = doc->source[i];
        if (e == '"' || e == '\\' || e == '/') c = (unsigned char)e;
        else if (e == 'b') c = '\b';
        else if (e == 'f') c = '\f';
        else if (e == 'n') c = '\n';
        else if (e == 'r') c = '\r';
        else if (e == 't') c = '\t';
        else if (e == 'u') {
            unsigned cp;
            if (i + 4 >= t->end || hex4(doc->source + i + 1, &cp) != 0) return -1;
            i += 4;
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
            if (append_utf8(out, cap, &used, cp) != 0) return -1;
            continue;
        } else return -1;
        if (used + 1 >= cap) return -1;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return 0;
}

int xz_json_get_string(const xz_json_doc *doc, int object, const char *key,
                       char *out, size_t capacity) {
    return decode_string(doc, xz_json_object_get(doc, object, key), out, capacity);
}

int xz_json_get_int(const xz_json_doc *doc, int object, const char *key, int *out) {
    int index = xz_json_object_get(doc, object, key);
    if (!doc || index < 0 || !out || doc->tokens[index].type != XZ_JSON_PRIMITIVE) return -1;
    const xz_json_token *t = &doc->tokens[index];
    size_t n = t->end - t->start;
    if (n == 0 || n >= 64) return -1;
    char temp[64];
    memcpy(temp, doc->source + t->start, n);
    temp[n] = '\0';
    errno = 0;
    char *end = NULL;
    long value = strtol(temp, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < -2147483647L - 1 || value > 2147483647L)
        return -1;
    *out = (int)value;
    return 0;
}
