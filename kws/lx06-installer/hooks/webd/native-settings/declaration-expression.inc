/* Same declaration-reader TU, after its original projections. */
#include "declaration-expression.h"
int decl_expr_match(const struct decl_table *t, enum decl_scope scope, const char *script,
                     const char *text, struct lx06_match_result *out, int *branch) {
    if(!t || !out || !branch || !text || scope<0 || scope>=DECL_SCOPES ||
       t->count[scope]>CFG_CANDIDATES) return CFG_INVALID;
    struct lx06_match_candidate *candidates=calloc(CFG_CANDIDATES,sizeof(*candidates));
    if(!candidates) return CFG_CAPACITY;
    int rc=CFG_OK, selected=-1;
    for(size_t i=0;i<t->count[scope];i++) {
        rc=decl_candidate(t,scope,i,script,&candidates[i]); if(rc) break;
    }
    struct lx06_match_result result;
    if(!rc) rc=lx06_expr_template_match(text,candidates,(int)t->count[scope],&result,
                                       &selected,EXPR_MATCH_STEPS);
    if(!rc) { *out=result; *branch=selected; }
    free(candidates); return rc;
}
int decl_expr_prepare_control(const struct decl_table *t, const char *script, const char *text,
                               struct decl_expr_control *out) {
    if(!t || !t->source || !out) return CFG_INVALID;
    struct decl_expr_control *next=calloc(1,sizeof(*next));
    if(!next) return CFG_CAPACITY;
    int rc=decl_expr_match(t,DECL_CTL,script,text,&next->match,&next->branch);
    if(rc) goto done;
    if(next->match.kind!=LX06_MATCH_EXACT && next->match.kind!=LX06_MATCH_TEMPLATE) goto accept;
    if(next->match.index<0) { rc=CFG_INVALID; goto done; }
    const char *raw=decl_ctl_row(t,(size_t)next->match.index);
    struct cfg_row row;
    if(!raw) { rc=CFG_INVALID; goto done; }
    rc=cfg_parse_row(raw,&row); if(rc) goto done;
    /* cfg and table are borrowed unchanged for this synchronous call. */
    if(row.ref_count!=(size_t)next->match.var_count) { rc=CFG_INVALID; goto done; }
    for(size_t i=0;i<row.arg_count;i++) {
        const struct cfg_arg *arg=&row.args[i];
        const char *value=raw+arg->offset; size_t n=arg->length;
        if(*arg->ref) {
            int v=0;
            while(v<next->match.var_count && strcmp(arg->ref,next->match.vars[v].name)) v++;
            if(v==next->match.var_count) { rc=CFG_INVALID; goto done; }
            value=next->match.vars[v].value; n=strlen(value);
        }
        if(n>=sizeof(next->params[i].value)) { rc=CFG_CAPACITY; goto done; }
        strcpy(next->params[i].key,arg->key);
        memcpy(next->params[i].value,value,n); /* calloc supplies terminator */
    }
    strcpy(next->op,row.op); next->count=row.arg_count;
accept:
    *out=*next;
done:
    memset(next,0,sizeof(*next)); free(next); return rc;
}
