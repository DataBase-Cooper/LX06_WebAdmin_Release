/* Included in the original matcher TU, after its declaration compiler.
 * Conditions are lowered to its original segments; ONLY match_from compares
 * user text or chooses capture boundaries. The visitor combines its matches. */
#include "template-runtime.h"

static size_t expr_runtime_strlen(const char *s, size_t cap) {
    size_t n=0; while(n<cap && s[n]) n++; return n;
}

struct expr_runtime_hit {
    int fixed, branch, var_count, incomplete;
    struct lx06_match_var vars[4];
};
struct expr_runtime {
    const struct expr_program *program;
    const char *norm;
    const struct raw_map *map;
    unsigned *remaining;
    int exhausted, bytes;
    struct expr_branch branch;
    struct lx06_pat pats[EXPR_CONDITIONS];
    struct lx06_match_walk walks[EXPR_CONDITIONS];
    unsigned char coverage[513];
    struct lx06_match_var vars[4];
    int var_count;
    struct expr_runtime_hit best;
};
struct expr_visit { struct expr_runtime *r; unsigned condition; };

static int expr_lower(const struct expr_program *p, const struct expr_condition *c,
                       struct lx06_pat *pat) {
    memset(pat,0,sizeof(*pat));
    for(unsigned i=0;i<c->count;i++) {
        const struct expr_node *node=&p->nodes[c->atoms[i]];
        if(node->kind==EXPR_LITERAL) {
            struct lx06_seg *seg;
            if(!pat->segs || pat->seg[pat->segs-1].var) {
                if(pat->segs==9) return EXPR_CAPACITY;
                seg=&pat->seg[pat->segs++];
            } else seg=&pat->seg[pat->segs-1];
            if((size_t)seg->lit_bytes+node->length>=sizeof(seg->lit) ||
               strlen(pat->skeleton)+node->length>=sizeof(pat->skeleton)) return EXPR_CAPACITY;
            const char *lit=p->literals+node->offset;
            memcpy(seg->lit+seg->lit_bytes,lit,node->length);
            seg->lit_bytes+=node->length;
            strcat(pat->skeleton,lit);
            pat->fixed+=char_count(lit,node->length);
        } else {
            if(pat->segs==9) return EXPR_CAPACITY;
            struct lx06_seg *seg=&pat->seg[pat->segs++];
            seg->var=1; seg->vmin=1;
            strcpy(seg->name,p->names[node->slot]);
            seg->vmax=var_limit(seg->name); pat->vars++;
        }
    }
    pat->ok=1; return EXPR_OK;
}
static void expr_runtime_condition(struct expr_runtime *, unsigned);
/* Returning zero tells the original recursive walker to continue enumeration.
 * Its shortest-first captures + condition source order settle equal-score
 * split ties. Higher union coverage always wins before that tie breaker. */
static int expr_runtime_accept(void *ctx, const struct lx06_match_var *vars, int n,
                                const unsigned char *covered) {
    struct expr_visit *v=ctx; struct expr_runtime *r=v->r;
    int old=r->var_count;
    if(old+n>4) { r->exhausted=1; return 0; }
    memcpy(r->vars+old,vars,(size_t)n*sizeof(*vars)); r->var_count+=n;
    for(int i=0;i<r->bytes;i++) r->coverage[i]+=covered[i];
    expr_runtime_condition(r,v->condition+1);
    for(int i=0;i<r->bytes;i++) r->coverage[i]-=covered[i];
    r->var_count=old; return 0;
}
static void expr_runtime_condition(struct expr_runtime *r, unsigned condition) {
    if(r->exhausted) return;
    if(condition==r->branch.count) {
        int score=0;
        for(int i=0;i<r->bytes;) {
            if(r->coverage[i]) score++;
            i+=utf8_len((const unsigned char *)r->norm+i);
        }
        if(score>r->best.fixed) {
            r->best.fixed=score; r->best.var_count=r->var_count;
            memset(r->best.vars,0,sizeof(r->best.vars));
            memcpy(r->best.vars,r->vars,(size_t)r->var_count*sizeof(*r->vars));
        }
        return;
    }
    struct expr_visit v={r,condition};
    struct lx06_match_walk *walk=&r->walks[condition];
    memset(walk,0,sizeof(*walk));
    walk->remaining=r->remaining; walk->exhausted=&r->exhausted;
    walk->accept=expr_runtime_accept; walk->ctx=&v;
    struct lx06_match_var vars[4]={0}; int n=0;
    (void)match_from(r->norm,0,r->bytes,&r->pats[condition],0,vars,&n,r->map,walk);
}
/* The compiled-object slice supplies the SAME program and scratch here. It
 * does not reparse source or introduce another text matcher/arbitrator. */
static int expr_runtime_program_limits(const struct expr_program *p, struct expr_runtime *r,
                               const char *norm, const struct raw_map *map,
                               unsigned *remaining, struct expr_runtime_hit *out,const struct expr_list_bounds *limits) {
    int rc=EXPR_OK;
    memset(r,0,sizeof(*r));
    r->program=p; r->norm=norm; r->map=map; r->remaining=remaining;
    r->bytes=(int)strlen(norm); r->best.fixed=-1; r->best.branch=-1;
    for(unsigned b=0;b<p->branches;b++) {
        rc=lx06_expr_branch(p,b,&r->branch); if(rc) break;
        for(unsigned c=0;c<r->branch.count;c++) {
            rc=expr_lower(p,&r->branch.conditions[c],&r->pats[c]); if(rc) break;
            if(limits)for(int i=0;i<r->pats[c].segs;i++) {
                struct lx06_seg *seg=&r->pats[c].seg[i];
                for(unsigned slot=0;seg->var&&slot<p->slots;slot++)if(p->lists[slot]&&!strcmp(seg->name,p->names[slot])) {
                    if(!(limits->mask&(1U<<slot))||!limits->min[slot]||limits->min[slot]>limits->max[slot]||limits->max[slot]>256)return EXPR_INVALID;
                    seg->vmin=limits->min[slot];seg->vmax=limits->max[slot];
                }
            }
        }
        if(rc) break;
        /* Incomplete is a single full sequence with its nonempty captures
         * omitted, never a successful zero-length capture or AND partial hit. */
        if(r->branch.count==1 && r->pats[0].vars && !strcmp(norm,r->pats[0].skeleton))
            r->best.incomplete=1;
        int score=r->best.fixed;
        expr_runtime_condition(r,0);
        if(r->exhausted) { rc=EXPR_CAPACITY; break; }
        if(r->best.fixed>score) r->best.branch=(int)b;
    }
    if(!rc) *out=r->best;
    return rc;
}
static int expr_runtime_program(const struct expr_program *p,struct expr_runtime *r,const char *norm,
        const struct raw_map *map,unsigned *remaining,struct expr_runtime_hit *out) {
    return expr_runtime_program_limits(p,r,norm,map,remaining,out,NULL);
}
static int expr_runtime_match_limits(const char *source, const char *norm, const struct raw_map *map,
                               unsigned *remaining, struct expr_runtime_hit *out,const struct expr_list_bounds *limits) {
    struct expr_program *p=calloc(1,sizeof(*p));
    struct expr_runtime *r=calloc(1,sizeof(*r));
    if(!p || !r) { free(p); free(r); return EXPR_CAPACITY; }
    int rc=lx06_expr_compile(source,strlen(source),p);
    if(!rc) rc=expr_runtime_program_limits(p,r,norm,map,remaining,out,limits);
    memset(r,0,sizeof(*r)); free(r); memset(p,0,sizeof(*p)); free(p); return rc;
}
static int expr_runtime_match(const char *source,const char *norm,const struct raw_map *map,
        unsigned *remaining,struct expr_runtime_hit *out) {
    return expr_runtime_match_limits(source,norm,map,remaining,out,NULL);
}
/* Validate every row before arbitration, including rows after an exact hit.
 * No silently skipped syntax or partial list execution. Constant scratch
 * program rather than a 1024-program allocation. */
static int expr_runtime_validate_lists(const struct lx06_match_candidate *cands, int count,
                                  unsigned char *exact,expr_list_approve approve,void *ctx,struct expr_list_bounds *limits) {
    struct expr_program *p=calloc(1,sizeof(*p));
    if(!p) return EXPR_CAPACITY;
    int rc=EXPR_OK;
    for(int i=0;i<count;i++) {
        if(!cands[i].phrase) { rc=EXPR_INVALID; break; }
        size_t n=expr_runtime_strlen(cands[i].phrase,EXPR_BYTES+1);
        rc=lx06_expr_compile(cands[i].phrase,n,p); if(rc) break;
        unsigned lists=0;for(unsigned s=0;s<p->slots;s++)if(p->lists[s])lists|=1U<<s;
        if(lists) {
            if(!approve||!limits){rc=EXPR_LIST_PENDING;break;}
            if(approve(ctx,&cands[i],p,&limits[i])||limits[i].mask!=lists){rc=EXPR_INVALID;break;}
            for(unsigned s=0;s<p->slots;s++)if(p->lists[s]&&(!limits[i].min[s]||
               limits[i].min[s]>limits[i].max[s]||limits[i].max[s]>256)){rc=EXPR_INVALID;break;}
        }
        if(rc) break;
        exact[i]=(unsigned char)(!p->syntax && !p->slots);
    }
    memset(p,0,sizeof(*p)); free(p); return rc;
}
static int expr_runtime_validate(const struct lx06_match_candidate *cands,int count,unsigned char *exact) {
    return expr_runtime_validate_lists(cands,count,exact,NULL,NULL,NULL);
}
