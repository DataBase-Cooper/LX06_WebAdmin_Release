/* Private extension of the SAME cfg parser. Included after its original RHS
 * parser, which remains the only parser of op and key=value declarations. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision12 effective declarations are fixture-only"
#endif
#include "template-matcher.h"
#include "template-runtime.h"
#include "ctl-list-approval.h"

static int cfg_expr_scalar(const char *source, size_t n, struct expr_program *p) {
    int rc=lx06_expr_compile(source,n,p);
    if(rc) return rc;
    for(unsigned i=0;i<p->slots;i++) if(p->lists[i]) return EXPR_LIST_PENDING;
    return CFG_OK;
}
static int cfg_expr_list(const char *source, size_t n, int mode, size_t *count) {
    struct expr_span *spans=calloc(EXPR_ITEMS,sizeof(*spans));
    struct expr_program *p=malloc(sizeof(*p));
    if(!spans || !p) { free(spans); free(p); return CFG_CAPACITY; }
    size_t used=0;
    int rc=lx06_expr_split(source,n,mode,spans,EXPR_ITEMS,&used);
    for(size_t i=0;!rc && i<used;i++)
        rc=cfg_expr_scalar(source+spans[i].offset,spans[i].length,p);
    if(!rc) *count=used;
    free(spans); free(p); return rc;
}
static int cfg_expr_channels_startup(const struct cfg *c,cfg_startup_approve approve,void *ctx) {
    for(size_t i=0;i<c->count;i++) {
        const char *key=c->entries[i].key;
        size_t n=strlen(key);
        int mode=!strcmp(key,"小爱通道唤醒词")?',':'|';
        if(strcmp(key,"小爱通道唤醒词") && strcmp(key,"LX06-LOCAL-CHANNEL") &&
           (n<9 || strcmp(key+n-9,"-PLAY-TPL"))) continue;
        const char *v=c->arena+c->entries[i].offset;
        if(*v) {
            if(!approve||(strcmp(key,"小爱通道唤醒词")&&strcmp(key,"LX06-LOCAL-CHANNEL"))) {
                size_t count;int rc=cfg_expr_list(v,strlen(v),mode,&count);if(rc)return rc;
            } else {
                struct expr_span *spans=calloc(EXPR_ITEMS,sizeof(*spans));
                struct expr_program *p=malloc(sizeof(*p));size_t count=0;
                if(!spans||!p){free(spans);free(p);return CFG_CAPACITY;}
                int rc=lx06_expr_split(v,strlen(v),mode,spans,EXPR_ITEMS,&count);
                for(size_t row=0;!rc&&row<count;row++) {
                    rc=lx06_expr_compile(v+spans[row].offset,spans[row].length,p);
                    unsigned lists=0;for(unsigned s=0;!rc&&s<p->slots;s++)if(p->lists[s])lists|=1U<<s;
                    if(!rc&&lists){struct expr_list_bounds bounds={0};if(approve(ctx,c,key,(unsigned)row,p,&bounds)||bounds.mask!=lists)rc=CFG_INVALID;}
                }
                free(spans);free(p);if(rc)return rc;
            }
        }
    }
    return CFG_OK;
}
static int cfg_expr_channels(const struct cfg *c) {return cfg_expr_channels_startup(c,NULL,NULL);}
int cfg_validate_startup_channels(const struct cfg *c,cfg_startup_approve approve,void *ctx) {
    return c&&approve?cfg_expr_channels_startup(c,approve,ctx):CFG_INVALID;
}

static int cfg_parse_row_impl(const char *raw, struct cfg_row *out,unsigned *list_mask) {
    if(!raw || !out) return CFG_INVALID;
    size_t n=strnlen(raw,CFG_BYTES+1), used=0;
    if(n>CFG_BYTES) return CFG_CAPACITY;
    if(!cfg_utf8_valid(raw,n) || strpbrk(raw,"\r\n")) return CFG_INVALID;
    struct expr_span sides[2];
    int rc=lx06_expr_split(raw,n,'=',sides,2,&used);
    if(rc) return rc;
    struct expr_span *phrases=calloc(EXPR_ITEMS,sizeof(*phrases));
    struct expr_program *p=malloc(sizeof(*p));
    char *synthetic=calloc(CFG_BYTES+1,1);
    if(!phrases || !p || !synthetic) {
        free(phrases); free(p); free(synthetic); return CFG_CAPACITY;
    }
    size_t count=0;
    rc=lx06_expr_split(raw,sides[0].length,'|',phrases,EXPR_ITEMS,&count);
    struct cfg_row next={0};unsigned previous_lists=0;
    for(size_t i=0;!rc && i<count;i++) {
        const char *current=raw+phrases[i].offset;
        size_t current_n=phrases[i].length;
        while(current_n && ascii_space(*current)) { current++; current_n--; }
        while(current_n && ascii_space(current[current_n-1])) current_n--;
        for(size_t j=0;j<i;j++) {
            const char *prior=raw+phrases[j].offset; size_t prior_n=phrases[j].length;
            while(prior_n && ascii_space(*prior)) { prior++; prior_n--; }
            while(prior_n && ascii_space(prior[prior_n-1])) prior_n--;
            if(current_n==prior_n && !memcmp(current,prior,current_n)) { rc=CFG_INVALID; break; }
        }
        if(rc) break;
        rc=list_mask?lx06_expr_compile(raw+phrases[i].offset,phrases[i].length,p):
            cfg_expr_scalar(raw+phrases[i].offset,phrases[i].length,p);
        if(rc) break;
        /* Current flat CTL RHS is shared by every complete B. Require the
         * same available names on every B; no invented completion/unused
         * declaration and no borrowing a capture from an unselected B. */
        unsigned mask=(1u<<p->slots)-1u;
        for(unsigned b=0;b<p->branches;b++) {
            struct expr_branch branch;
            rc=lx06_expr_branch(p,b,&branch);
            if(rc || branch.slot_mask!=mask) { if(!rc) rc=CFG_INVALID; break; }
        }
        if(rc) break;
        size_t at=0;
        if(!p->slots) synthetic[at++]='X';
        for(unsigned s=0;s<p->slots;s++) {
            if(s) synthetic[at++]='X';
            synthetic[at++]='{';
            size_t k=strlen(p->names[s]); memcpy(synthetic+at,p->names[s],k); at+=k;
            synthetic[at++]='}';
        }
        memcpy(synthetic+at," => ",4); at+=4;
        const char *rhs=raw+sides[1].offset;
        size_t rhs_n=sides[1].length;
        /* Strip only ASCII outer whitespace. The original RHS parser owns
         * the grammar; captured text never passes through this parser. */
        while(rhs_n && ascii_space(*rhs)) { rhs++; rhs_n--; }
        while(rhs_n && ascii_space(rhs[rhs_n-1])) rhs_n--;
        if(at+rhs_n>CFG_BYTES) { rc=CFG_CAPACITY; break; }
        memcpy(synthetic+at,rhs,rhs_n); synthetic[at+rhs_n]=0;
        struct cfg_row row;
#ifdef LX06_PARAMETER_COMPLETION_FIXTURE
        rc=list_mask?cfg_legacy_parse_row_completion(synthetic,&row):cfg_legacy_parse_row(synthetic,&row);
#else
        rc=cfg_legacy_parse_row(synthetic,&row);
#endif
        if(rc) break;
        if(i && !same_vars(next.refs,next.ref_count,row.refs,row.ref_count)) { rc=CFG_INVALID; break; }
        for(size_t a=0;a<row.arg_count;a++)
            row.args[a].offset=(size_t)(rhs-raw)+(row.args[a].offset-at);
        unsigned lists=0;
        for(unsigned slot=0;slot<p->slots;slot++)if(p->lists[slot]) {
            size_t r;for(r=0;r<row.ref_count;r++)if(!strcmp(p->names[slot],row.refs[r]))break;
            if(r==row.ref_count){rc=CFG_INVALID;break;}
            lists|=1U<<r;
        }
        if(rc)break;
        if(i)for(size_t r=0;r<row.ref_count;r++) {
            size_t old;for(old=0;old<next.ref_count;old++)if(!strcmp(row.refs[r],next.refs[old]))break;
            if(old==next.ref_count||!!(lists&(1U<<r))!=!!(previous_lists&(1U<<old))){rc=CFG_INVALID;break;}
        }
        if(rc)break;
#ifdef LX06_PARAMETER_COMPLETION_FIXTURE
        for(size_t a=0;a<row.arg_count;a++)if(row.args[a].ref[0]) {
            size_t r;for(r=0;r<row.ref_count;r++)if(!strcmp(row.args[a].ref,row.refs[r]))break;
            if(r==row.ref_count)lists|=CFG_ROW_COMPLETION;
        }
#endif
        previous_lists=lists;next=row;
    }
    if(!rc) { next.phrase_count=count; *out=next;if(list_mask)*list_mask=previous_lists; }
    memset(synthetic,0,CFG_BYTES+1); free(synthetic); free(p); free(phrases); return rc;
}

int cfg_parse_row(const char *raw,struct cfg_row *out) {
    return cfg_parse_row_impl(raw,out,NULL);
}
int cfg_parse_row_scoped(const char *key,const char *raw,struct cfg_row *out,cfg_list_approve approve,void *ctx) {
    if(!out)return CFG_INVALID;
    if(!approve)return cfg_parse_row(raw,out);
    struct cfg_row next;unsigned lists=0;
    int rc=cfg_parse_row_impl(raw,&next,&lists);
    if(rc)return rc;
    if(lists&&(!key||approve(ctx,key,raw,&next,lists)))return CFG_INVALID;
    *out=next;return CFG_OK;
}
