/* Host-only static-branch positives. Reuse the original compiler, lowering,
 * normalization and expr_runtime_condition -> match_from visitor. There is
 * no source reconstruction, text comparator or replacement arbitration. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "static branch review is fixture-only"
#endif
struct gpb_hits { uint64_t matched, inspected; unsigned branches; };
size_t gpb_hits_size(void) {return sizeof(struct gpb_hits);}
struct gpb_scratch {
    struct expr_program program;
    struct expr_runtime runtime;
    struct raw_map map;
    char copy[513], norm[513];
};
int gpb_static(const char *phrase,const char *word,unsigned budget,struct gpb_hits *out) {
    if(!phrase||!word||!out||!budget||budget>EXPR_MATCH_STEPS) return EXPR_INVALID;
    size_t n=expr_runtime_strlen(word,513);
    if(n>512) return EXPR_CAPACITY;
    struct gpb_scratch *s=calloc(1,sizeof(*s));
    if(!s) return EXPR_CAPACITY;
    memcpy(s->copy,word,n);s->copy[n]=0;
    int rc=EXPR_INVALID;
    struct gpb_hits next={0};
    if(!expr_valid_utf8(s->copy,n)) goto done;
    struct expr_program *p=&s->program;
    rc=lx06_expr_compile(phrase,expr_runtime_strlen(phrase,EXPR_BYTES+1),p);
    if(rc) goto done;
    /* Preserve whole-program list rejection, even for a static OR arm. */
    for(unsigned i=0;i<p->slots;i++) if(p->lists[i]) {rc=EXPR_LIST_PENDING;goto done;}
    next.branches=p->branches;
    int valid=mapped_normalize(s->copy,s->norm,&s->map);
    for(unsigned b=0;b<p->branches;b++) {
        struct expr_runtime *r=&s->runtime;
        ats_static_wipe(r,sizeof(*r));
        rc=lx06_expr_branch(p,b,&r->branch);
        if(rc) goto done;
        if(r->branch.slot_mask) continue;
        for(unsigned c=0;c<r->branch.count;c++) {
            rc=expr_lower(p,&r->branch.conditions[c],&r->pats[c]);
            if(rc) goto done;
        }
        next.inspected|=UINT64_C(1)<<b;
        if(!valid) continue; /* same empty/overlength miss as original runtime */
        r->program=p;r->norm=s->norm;r->map=&s->map;r->remaining=&budget;
        r->bytes=(int)strlen(s->norm);r->best.fixed=-1;r->best.branch=-1;
        expr_runtime_condition(r,0);
        if(r->exhausted) {rc=EXPR_CAPACITY;goto done;}
        if(r->best.fixed>=0) next.matched|=UINT64_C(1)<<b;
    }
    *out=next;
done:
    ats_static_wipe(s,sizeof(*s));free(s);
    return rc;
}
