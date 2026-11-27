/* Included ONLY by the explicit patch to the one original matcher. Reuses its
 * strict UTF-8 decoder and literal normalizer; contains no recognition engine. */
#include "template-expression.h"
#include <stdlib.h>

#define EXPR_MAGIC UINT32_C(0x45585031)
struct expr_parser {
    const char *s;
    size_t n, at;
    unsigned depth;
    int error;
    struct expr_program *p;
};
static int expr_valid_utf8(const char *copy, size_t n) {
    for(size_t at=0;at<n;) {
        int k=utf8_len((const unsigned char *)copy+at);
        if(!k || (unsigned char)copy[at]<9 || ((unsigned char)copy[at]>13 && (unsigned char)copy[at]<32)) return 0;
        at+=(size_t)k;
    }
    return 1;
}
static unsigned expr_cp(const char *s, int k) {
    unsigned cp=(unsigned char)*s;
    if(k>1) {
        cp &= (1u<<(7-k))-1;
        for(int i=1;i<k;i++) cp=(cp<<6)|((unsigned char)s[i]&63);
    }
    return cp;
}
static int expr_space(unsigned cp) {
    return (cp>=9 && cp<=13) || cp==32 || cp==0x85 || cp==0xa0 || cp==0x1680 ||
        (cp>=0x2000 && cp<=0x200a) || cp==0x2028 || cp==0x2029 || cp==0x202f ||
        cp==0x205f || cp==0x3000;
}
static void expr_skip(struct expr_parser *c) {
    while(c->at<c->n) {
        int k=utf8_len((const unsigned char *)c->s+c->at);
        if(!k || !expr_space(expr_cp(c->s+c->at,k))) break;
        c->at+=(size_t)k;
    }
}
static int expr_boundary(const char *s, size_t at, size_t n) {
    if(at==n || s[at]=='(' || s[at]==')') return 1;
    int k=utf8_len((const unsigned char *)s+at);
    return k && expr_space(expr_cp(s+at,k));
}
static int expr_op(struct expr_parser *c, const char *op) {
    size_t k=strlen(op), before=c->at;
    if(c->n-c->at<k || memcmp(c->s+c->at,op,k) ||
       !expr_boundary(c->s,c->at+k,c->n)) return 0;
    if(before) {
        before--;
        while(before && ((unsigned char)c->s[before]&0xc0)==0x80) before--;
        if(!expr_boundary(c->s,before,c->n)) return 0;
    }
    return 1;
}
static int expr_new(struct expr_parser *c, unsigned kind, int left, int right) {
    if(c->error) return -1;
    if(c->p->count==EXPR_NODES) { c->error=EXPR_CAPACITY; return -1; }
    int id=c->p->count++;
    struct expr_node *v=&c->p->nodes[id];
    v->kind=(uint16_t)kind; v->branches=1;
    if(kind>=EXPR_SEQUENCE) {
        struct expr_node *a=&c->p->nodes[left], *b=&c->p->nodes[right];
        if(kind==EXPR_SEQUENCE && (a->has_and || b->has_and)) {
            c->error=EXPR_INVALID; return -1;
        }
        unsigned n=kind==EXPR_OR?a->branches+b->branches:a->branches*b->branches;
        if(n>EXPR_BRANCHES) { c->error=EXPR_CAPACITY; return -1; }
        v->left=(uint16_t)left; v->right=(uint16_t)right;
        v->branches=(uint16_t)n;
        v->has_and=(uint16_t)(kind==EXPR_AND || a->has_and || b->has_and);
    }
    return id;
}
static int expr_hex(struct expr_parser *c, unsigned *out) {
    if(c->n-c->at<4) return 0;
    unsigned value=0;
    for(int i=0;i<4;i++) {
        unsigned char ch=(unsigned char)c->s[c->at++]; unsigned x;
        if(ch>='0' && ch<='9') x=ch-'0';
        else if(ch>='a' && ch<='f') x=ch-'a'+10;
        else if(ch>='A' && ch<='F') x=ch-'A'+10;
        else return 0;
        value=value*16+x;
    }
    *out=value; return 1;
}
static int expr_put_cp(char *s, size_t *used, unsigned cp) {
    unsigned char bytes[4]; int n;
    if(!cp || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return 0;
    if(cp<0x80) { bytes[0]=(unsigned char)cp; n=1; }
    else if(cp<0x800) { bytes[0]=(unsigned char)(0xc0|(cp>>6)); bytes[1]=(unsigned char)(0x80|(cp&63)); n=2; }
    else if(cp<0x10000) {
        bytes[0]=(unsigned char)(0xe0|(cp>>12)); bytes[1]=(unsigned char)(0x80|((cp>>6)&63));
        bytes[2]=(unsigned char)(0x80|(cp&63)); n=3;
    } else {
        bytes[0]=(unsigned char)(0xf0|(cp>>18)); bytes[1]=(unsigned char)(0x80|((cp>>12)&63));
        bytes[2]=(unsigned char)(0x80|((cp>>6)&63)); bytes[3]=(unsigned char)(0x80|(cp&63)); n=4;
    }
    if(*used+(size_t)n>512) return 0;
    memcpy(s+*used,bytes,(size_t)n); *used+=(size_t)n; return 1;
}
static int expr_quoted(struct expr_parser *c, char raw[513]) {
    size_t used=0; c->at++;
    while(c->at<c->n) {
        unsigned char ch=(unsigned char)c->s[c->at++];
        if(ch=='"') { raw[used]=0; return 1; }
        if(ch<32) break;
        if(ch=='\\') {
            if(c->at==c->n) break;
            ch=(unsigned char)c->s[c->at++];
            if(ch=='u') {
                unsigned cp,lo;
                if(!expr_hex(c,&cp)) break;
                if(cp>=0xd800 && cp<=0xdbff) {
                    if(c->n-c->at<6 || c->s[c->at]!='\\' || c->s[c->at+1]!='u') break;
                    c->at+=2;
                    if(!expr_hex(c,&lo) || lo<0xdc00 || lo>0xdfff) break;
                    cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
                }
                if(!expr_put_cp(raw,&used,cp)) break;
                continue;
            }
            const char *keys="\"\\/bfnrt", *values="\"\\/\b\f\n\r\t";
            const char *pos=strchr(keys,ch);
            if(!pos) break;
            ch=(unsigned char)values[pos-keys];
        }
        if(used==512) { c->error=EXPR_CAPACITY; return 0; }
        raw[used++]=(char)ch;
    }
    c->error=EXPR_INVALID; return 0;
}
static int expr_literal(struct expr_parser *c) {
    char raw[513], norm[513]; size_t used=0;
    if(c->s[c->at]=='"') {
        c->p->syntax=1;
        if(!expr_quoted(c,raw)) return -1;
    } else {
        while(c->at<c->n) {
            unsigned char ch=(unsigned char)c->s[c->at];
            int k=utf8_len((const unsigned char *)c->s+c->at);
            if(expr_space(expr_cp(c->s+c->at,k)) || strchr("(){}\"\\|,&^$.*+?[]",ch)) break;
            if(used+(size_t)k>512) { c->error=EXPR_CAPACITY; return -1; }
            memcpy(raw+used,c->s+c->at,(size_t)k); used+=(size_t)k; c->at+=(size_t)k;
        }
        raw[used]=0;
    }
    struct raw_map map;
    if(!mapped_normalize(raw,norm,&map)) { c->error=EXPR_INVALID; return -1; }
    size_t n=strlen(norm)+1;
    if(n>sizeof(c->p->literals)-c->p->used) { c->error=EXPR_CAPACITY; return -1; }
    int id=expr_new(c,EXPR_LITERAL,0,0);
    if(id<0) return -1;
    struct expr_node *v=&c->p->nodes[id];
    v->offset=c->p->used; v->length=(uint16_t)(n-1);
    memcpy(c->p->literals+c->p->used,norm,n); c->p->used+=(uint16_t)n;
    return id;
}
static int expr_slot(struct expr_parser *c) {
    size_t start=++c->at;
    while(c->at<c->n && ((c->s[c->at]>='a' && c->s[c->at]<='z') || c->s[c->at]=='_')) c->at++;
    size_t n=c->at-start;
    int list=c->at<c->n && c->s[c->at]=='*';
    if(list) { c->at++; c->p->syntax=1; }
    if(!n || n>16 || c->at==c->n || c->s[c->at++]!='}') { c->error=EXPR_INVALID; return -1; }
    for(unsigned i=0;i<c->p->slots;i++) {
        if(strlen(c->p->names[i])==n && !memcmp(c->p->names[i],c->s+start,n)) {
            c->error=EXPR_INVALID; return -1;
        }
    }
    if(c->p->slots==EXPR_SLOTS) { c->error=EXPR_CAPACITY; return -1; }
    unsigned slot=c->p->slots++;
    memcpy(c->p->names[slot],c->s+start,n); c->p->lists[slot]=(unsigned char)list;
    int id=expr_new(c,EXPR_SLOT,0,0);
    if(id>=0) c->p->nodes[id].slot=(uint16_t)slot;
    return id;
}
static int expr_or(struct expr_parser *c);
static int expr_atom(struct expr_parser *c) {
    expr_skip(c);
    if(c->at==c->n || c->s[c->at]==')' || expr_op(c,"OR") || expr_op(c,"AND")) {
        c->error=EXPR_INVALID; return -1;
    }
    if(c->s[c->at]=='{') return expr_slot(c);
    if(c->s[c->at]=='(') {
        c->p->syntax=1;
        if(++c->depth>EXPR_DEPTH) { c->error=EXPR_CAPACITY; return -1; }
        c->at++; int id=expr_or(c); expr_skip(c); c->depth--;
        if(id<0) return -1;
        if(c->at==c->n || c->s[c->at++]!=')') { c->error=EXPR_INVALID; return -1; }
        return id;
    }
    return expr_literal(c);
}
static int expr_sequence(struct expr_parser *c) {
    int id=expr_atom(c);
    while(id>=0) {
        expr_skip(c);
        if(c->at==c->n || c->s[c->at]==')' || expr_op(c,"OR") || expr_op(c,"AND")) break;
        int right=expr_atom(c);
        if(right<0) return -1;
        id=expr_new(c,EXPR_SEQUENCE,id,right);
    }
    return id;
}
static int expr_and(struct expr_parser *c) {
    int id=expr_sequence(c);
    while(id>=0) {
        expr_skip(c); if(!expr_op(c,"AND")) break;
        c->p->syntax=1; c->at+=3;
        int right=expr_sequence(c); if(right<0) return -1;
        id=expr_new(c,EXPR_AND,id,right);
    }
    return id;
}
static int expr_or(struct expr_parser *c) {
    int id=expr_and(c);
    while(id>=0) {
        expr_skip(c); if(!expr_op(c,"OR")) break;
        c->p->syntax=1; c->at+=2;
        int right=expr_and(c); if(right<0) return -1;
        id=expr_new(c,EXPR_OR,id,right);
    }
    return id;
}
static int expr_expand(const struct expr_program *p, unsigned id, unsigned branch,
                       struct expr_branch *out, unsigned condition, unsigned depth) {
    if(id>=p->count || depth>EXPR_NODES) return EXPR_INVALID;
    const struct expr_node *v=&p->nodes[id];
    if(branch>=v->branches) return EXPR_INVALID;
    if(v->kind==EXPR_OR) {
        unsigned left=p->nodes[v->left].branches;
        return branch<left?expr_expand(p,v->left,branch,out,condition,depth+1):
            expr_expand(p,v->right,branch-left,out,condition,depth+1);
    }
    if(v->kind==EXPR_AND || v->kind==EXPR_SEQUENCE) {
        unsigned right=p->nodes[v->right].branches;
        int rc=expr_expand(p,v->left,branch/right,out,condition,depth+1);
        if(rc) return rc;
        if(v->kind==EXPR_AND) {
            if(out->count==EXPR_CONDITIONS) return EXPR_CAPACITY;
            condition=out->count++;
        }
        return expr_expand(p,v->right,branch%right,out,condition,depth+1);
    }
    struct expr_condition *part=&out->conditions[condition];
    if(part->count==EXPR_ATOMS) return EXPR_CAPACITY;
    if(v->kind==EXPR_SLOT) {
        if(part->count && p->nodes[part->atoms[part->count-1]].kind==EXPR_SLOT) return EXPR_INVALID;
        out->slot_mask|=(uint16_t)(1u<<v->slot);
        if(p->lists[v->slot]) out->list_mask|=(uint16_t)(1u<<v->slot);
    }
    part->atoms[part->count++]=(uint16_t)id; return EXPR_OK;
}
int lx06_expr_branch(const struct expr_program *p, unsigned index, struct expr_branch *out) {
    if(!p || !out || p->magic!=EXPR_MAGIC || index>=p->branches) return EXPR_INVALID;
    struct expr_branch next={0}; next.count=1;
    int rc=expr_expand(p,p->root,index,&next,0,0);
    if(!rc) *out=next;
    return rc;
}
int lx06_expr_compile(const char *source, size_t n, struct expr_program *out) {
    if(!source || !out || !n) return EXPR_INVALID;
    if(n>EXPR_BYTES) return EXPR_CAPACITY;
    if(memchr(source,0,n)) return EXPR_INVALID;
    /* Bounded NUL-terminated copy makes the original UTF-8 decoder safe even
     * for a truncated final multibyte sequence at a guard page boundary. */
    char *copy=calloc(n+1,1); if(!copy) return EXPR_CAPACITY;
    memcpy(copy,source,n);
    if(!expr_valid_utf8(copy,n)) { free(copy); return EXPR_INVALID; }
    struct expr_program *p=calloc(1,sizeof(*p));
    if(!p) { free(copy); return EXPR_CAPACITY; }
    struct expr_parser c={.s=copy,.n=n,.p=p};
    int root=expr_or(&c); expr_skip(&c);
    int rc=c.error;
    if(!rc && (root<0 || c.at!=n)) rc=EXPR_INVALID;
    if(!rc) {
        p->root=(uint16_t)root; p->branches=p->nodes[root].branches; p->magic=EXPR_MAGIC;
        for(unsigned i=0;i<p->branches;i++) {
            struct expr_branch b;
            rc=lx06_expr_branch(p,i,&b); if(rc) break;
        }
    }
    if(!rc) *out=*p;
    /* Declarations may include private defaults. Don't retain our copies. */
    memset(copy,0,n); free(copy); memset(p,0,sizeof(*p)); free(p);
    return rc;
}
size_t lx06_expr_program_size(void) { return sizeof(struct expr_program); }
size_t lx06_expr_branch_size(void) { return sizeof(struct expr_branch); }

int lx06_expr_split(const char *source, size_t n, int mode, struct expr_span *out,
                    size_t capacity, size_t *count) {
    if(!source || !out || !count || !n || (mode!='|' && mode!=',' && mode!='=')) return EXPR_INVALID;
    if(n>EXPR_LIST_BYTES || capacity>EXPR_ITEMS) return EXPR_CAPACITY;
    if(memchr(source,0,n)) return EXPR_INVALID;
    char *copy=calloc(n+1,1);
    struct expr_span *spans=calloc(EXPR_ITEMS,sizeof(*spans));
    if(!copy || !spans) { free(copy); free(spans); return EXPR_CAPACITY; }
    memcpy(copy,source,n);
    if(!expr_valid_utf8(copy,n)) { free(copy); free(spans); return EXPR_INVALID; }
    struct expr_parser c={.s=copy,.n=n};
    size_t start=0, used=0; unsigned depth=0; int rc=EXPR_OK;
    while(c.at<n) {
        int k=utf8_len((const unsigned char *)copy+c.at);
        if(!k || (unsigned char)copy[c.at]<9 || ((unsigned char)copy[c.at]>13 && (unsigned char)copy[c.at]<32)) {
            rc=EXPR_INVALID; break;
        }
        if(copy[c.at]=='"') {
            char decoded[513];
            if(!expr_quoted(&c,decoded)) { rc=c.error; break; }
            continue;
        }
        if(copy[c.at]=='(') {
            if(++depth>EXPR_DEPTH) { rc=EXPR_CAPACITY; break; }
        } else if(copy[c.at]==')') {
            if(!depth) { rc=EXPR_INVALID; break; }
            depth--;
        }
        int delimiter=!depth && (mode=='='?(copy[c.at]=='=' && c.at+1<n && copy[c.at+1]=='>'):copy[c.at]==mode);
        if(delimiter) {
            if(used==EXPR_ITEMS) { rc=EXPR_CAPACITY; break; }
            if(c.at==start || (mode=='=' && used)) { rc=EXPR_INVALID; break; }
            spans[used++]=(struct expr_span){(uint32_t)start,(uint32_t)(c.at-start)};
            c.at+=(mode=='='?2:1); start=c.at;
        } else c.at+=(size_t)k;
    }
    if(!rc && (depth || start==n || (mode=='=' && !used))) rc=EXPR_INVALID;
    if(!rc && used==EXPR_ITEMS) rc=EXPR_CAPACITY;
    if(!rc) spans[used++]=(struct expr_span){(uint32_t)start,(uint32_t)(n-start)};
    if(!rc && used>capacity) rc=EXPR_CAPACITY;
    if(!rc) { memcpy(out,spans,used*sizeof(*spans)); *count=used; }
    memset(copy,0,n); free(copy); free(spans); return rc;
}
