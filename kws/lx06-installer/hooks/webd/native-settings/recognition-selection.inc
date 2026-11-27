/* A-05 HOST ONLY. Include after ctl-selection.inc/recognition-catalog.inc.
 * Adapter for the SAME recognition audit and original write transaction.
 * Selection, metadata, records and absolute ancestors use the existing native
 * owner/selector; no second parser, writer, lock, model or permission service. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition selection is fixture-only"
#endif
struct rec_selection_scope {
    const struct ctl_selection_root *roots;
    struct ctl_store_root *const *ancestors;
    struct ctl_selection *selection;
};
static struct ctl_catalog *rec_selection_acquire(void *owner,size_t retained,void *ctx) {
    struct rec_selection_scope *s=ctx;
    if(s->selection) return NULL;
    s->selection=ctl_selection_acquire_budget(owner,s->roots,s->ancestors,retained);
    return s->selection?s->selection->catalog:NULL;
}
static int rec_selection_current(const struct ctl_catalog *c,void *ctx) {
    const struct rec_selection_scope *s=ctx;
    return s->selection&&s->selection->catalog==c?ctl_selection_current(s->selection):-1;
}
static int rec_selection_scripts(const struct ctl_catalog *c,size_t i,void *ctx) {
    const struct rec_selection_scope *s=ctx;
    if(!s->selection||s->selection->catalog!=c||i>=c->count) return -1;
    return s->selection->roots[s->selection->source[i]].dir;
}
static void rec_selection_release(struct ctl_catalog *c,void *ctx) {
    struct rec_selection_scope *s=ctx;
    (void)c;ctl_selection_release(s->selection);s->selection=NULL;
}
int rec_selection_apply_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],
        const struct ctl_selection_root roots[3],struct ctl_store_root *const ancestors[3]) {
    if(!roots||!ancestors) return -1;
    struct rec_selection_scope s={roots,ancestors,NULL};
    const struct rec_catalog_provider provider={rec_selection_acquire,rec_selection_current,
        rec_selection_scripts,rec_selection_release,&s};
    return rec_catalog_apply_loaded_impl(scripts,parent,entry,name,revision,field,app,
        changes,changes_length,ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,
        audit,audit_ctx,gate,gate_ctx,ctl_out,rec_out,&provider);
}
