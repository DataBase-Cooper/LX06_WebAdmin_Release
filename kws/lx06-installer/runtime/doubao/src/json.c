#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    xz_json_doc *doc;
    size_t position;
#ifdef XZ_JSON_ENABLE_EVENTS
    const xz_json_events *events;
    size_t length;
    int token_count,failed;
    xz_json_type root_type;
#endif
} parser;

static size_t parser_length(const parser *p) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if (p->events) return p->length;
#endif
    return p->doc->length;
}
static unsigned char parser_byte(parser *p,size_t offset) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if (p->events) {
        if (p->failed || offset>=p->length) {p->failed=1;return 0;}
        int value=p->events->byte(p->events->context,offset);
        if (value<0 || value>255) {p->failed=1;return 0;}
        return (unsigned char)value;
    }
#endif
    return (unsigned char)p->doc->source[offset];
}
static int parser_end(parser *p,int index,size_t end) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if (p->events) {
        if (p->failed || p->events->close(p->events->context,index,end)) {p->failed=1;return -1;}
        return 0;
    }
#endif
    p->doc->tokens[index].end=end;return 0;
}
static void skip_space(parser *p) {
    while (p->position < parser_length(p) &&
           isspace(parser_byte(p,p->position))) p->position++;
}

static int add_token(parser *p, xz_json_type type, size_t start, size_t end, int parent) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if (p->events) {
        if (p->failed || p->token_count>=XZ_JSON_MAX_TOKENS) return -1;
        int index=p->token_count++;
        if (!index) p->root_type=type;
        if (p->events->token(p->events->context,index,type,start,end,parent)) {p->failed=1;return -1;}
        return index;
    }
#endif
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
    if (p->position >= parser_length(p) || parser_byte(p,p->position) != '"') return -1;
    p->position++;
    size_t start = p->position;
    while (p->position < parser_length(p)) {
        unsigned char c = parser_byte(p,p->position);
        if (c == '"') {
            size_t end = p->position++;
            return add_token(p, XZ_JSON_STRING, start, end, parent);
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            p->position++;
            if (p->position >= parser_length(p)) return -1;
            char e = (char)parser_byte(p,p->position++);
            if (e == 'u') {
                if (p->position + 4 > parser_length(p)) return -1;
                for (int i = 0; i < 4; ++i)
                    if (!isxdigit(parser_byte(p,p->position + (size_t)i))) return -1;
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
    if (p->position < parser_length(p) && parser_byte(p,p->position) == close) {
        if (parser_end(p,index,++p->position)) return -1;
        return index;
    }
    for (;;) {
        skip_space(p);
        if (type == XZ_JSON_OBJECT) {
            if (parse_string(p, index) < 0) return -1;
            skip_space(p);
            if (p->position >= parser_length(p) || parser_byte(p,p->position++) != ':') return -1;
            skip_space(p);
            if (parse_value(p, index, depth + 1) < 0) return -1;
        } else {
            if (parse_value(p, index, depth + 1) < 0) return -1;
        }
        skip_space(p);
        if (p->position >= parser_length(p)) return -1;
        char c = (char)parser_byte(p,p->position++);
        if (c == close) {
            if (parser_end(p,index,p->position)) return -1;
            return index;
        }
        if (c != ',') return -1;
    }
    (void)open;
}

static int parse_value(parser *p, int parent, unsigned depth) {
    if (depth > 64) return -1;
    skip_space(p);
    if (p->position >= parser_length(p)) return -1;
    char c = (char)parser_byte(p,p->position);
    if (c == '"') return parse_string(p, parent);
    if (c == '{') return parse_container(p, parent, depth, '{', '}', XZ_JSON_OBJECT);
    if (c == '[') return parse_container(p, parent, depth, '[', ']', XZ_JSON_ARRAY);
    size_t start = p->position;
    while (p->position < parser_length(p)) {
        c = (char)parser_byte(p,p->position);
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
    parser p = {.doc=doc,.position=0};
    doc->root = parse_value(&p, -1, 0);
    if (doc->root < 0) return -1;
    skip_space(&p);
    return p.position == length && doc->tokens[doc->root].type == XZ_JSON_OBJECT ? 0 : -1;
}

#ifdef XZ_JSON_ENABLE_EVENTS
int xz_json_visit(const xz_json_events *events,size_t length) {
    if (!events || !events->byte || !events->token || !events->close || !length) return -1;
    parser p={.events=events,.length=length};
    if (parse_value(&p,-1,0)<0 || p.failed) return -1;
    skip_space(&p);
    return !p.failed && p.position==length && p.root_type==XZ_JSON_OBJECT?0:-1;
}
#endif

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

typedef struct {
    const xz_json_doc *doc;
    char *out;size_t cap,used;
#ifdef XZ_JSON_ENABLE_EVENTS
    const xz_json_events *events;
    int (*write)(void *,const char *,size_t);
    void *context;int strict;
#endif
} string_decoder;
static int decoder_byte(const string_decoder *d,size_t offset) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if(d->events){int b=d->events->byte(d->events->context,offset);return b>=0&&b<=255?b:-1;}
#endif
    return (unsigned char)d->doc->source[offset];
}
static int decoder_write(string_decoder *d,const char *bytes,size_t n) {
#ifdef XZ_JSON_ENABLE_EVENTS
    if(d->events){
        if(n>(size_t)-1-d->used||d->write(d->context,bytes,n))return -1;
        d->used+=n;return 0;
    }
#endif
    if(n>=d->cap-d->used)return -1;
    memcpy(d->out+d->used,bytes,n);d->used+=n;return 0;
}
/* One decoding implementation for original buffered and opt-in event sinks. */
static int decode_span(string_decoder *d,size_t start,size_t end) {
    for(size_t i=start;i<end;i++){
        int value=decoder_byte(d,i);if(value<0)return -1;
        unsigned char c=(unsigned char)value;
        if(c!='\\'){
            if(decoder_write(d,(const char *)&c,1))return -1;
            continue;
        }
        if(++i>=end)return -1;
        value=decoder_byte(d,i);if(value<0)return -1;char e=(char)value;
        if(e=='"'||e=='\\'||e=='/')c=(unsigned char)e;
        else if(e=='b')c='\b';else if(e=='f')c='\f';else if(e=='n')c='\n';
        else if(e=='r')c='\r';else if(e=='t')c='\t';
        else if(e=='u'){
            unsigned cp;char digits[4],encoded[5];size_t n=0;
            if(i+4>=end)return -1;
            for(unsigned k=0;k<4;k++){value=decoder_byte(d,i+1+k);if(value<0)return -1;digits[k]=(char)value;}
            if(hex4(digits,&cp))return -1;
            i+=4;
#ifdef XZ_JSON_ENABLE_EVENTS
            if(d->events&&d->strict&&(!cp||(cp>=0xd800&&cp<=0xdfff)))return -1;
#endif
            if(cp>=0xd800&&cp<=0xdfff)cp=0xfffd;
            if(append_utf8(encoded,sizeof(encoded),&n,cp)||decoder_write(d,encoded,n))return -1;
            continue;
        }else return -1;
        if(decoder_write(d,(const char *)&c,1))return -1;
    }
    if(d->out)d->out[d->used]=0;
    return 0;
}
static int decode_string(const xz_json_doc *doc,int token,char *out,size_t cap) {
    if(!doc||token<0||token>=doc->token_count||!out||!cap||doc->tokens[token].type!=XZ_JSON_STRING)return -1;
    string_decoder d={.doc=doc,.out=out,.cap=cap};
    return decode_span(&d,doc->tokens[token].start,doc->tokens[token].end);
}
#ifdef XZ_JSON_ENABLE_EVENTS
int xz_json_decode_visit(const xz_json_events *events,size_t start,size_t end,int strict,
    int (*write)(void *,const char *,size_t),void *context,size_t *decoded_length) {
    if(decoded_length)*decoded_length=0;
    if(!events||!events->byte||!write||!decoded_length||start>end||(strict!=0&&strict!=1))return -1;
    string_decoder d={.events=events,.write=write,.context=context,.strict=strict};
    if(decode_span(&d,start,end))return -1;*decoded_length=d.used;return 0;
}
#endif

int xz_json_decode_token(const xz_json_doc *doc,int token,char *out,size_t capacity) {
    return decode_string(doc,token,out,capacity);
}
int xz_json_hex4(const char *source,unsigned *out) { return hex4(source,out); }

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
