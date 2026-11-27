/* A-05 HOST ONLY. Include after ctl-catalog.inc and recognition-owner.inc.
 * Default: every managed script in the SAME anchored flat directory/store.
 * An optional native provider reuses the retained two-directory selector.
 * Neither is a production registration or a model owner.
 * No new parser, matcher, store, lock, runtime permission or writer. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) || !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "recognition catalog requires the fixture complete-script parser"
#endif
/* Private synchronous adapter only. All operations must use the same borrowed
 * original write scope/lock; the provider lives through the writer's return.
 * No returned pointer or callback grants runtime permission. */
struct rec_catalog_provider {
    struct ctl_catalog *(*acquire)(void *,size_t,void *);
    int (*current)(const struct ctl_catalog *,void *);
    int (*scripts)(const struct ctl_catalog *,size_t,void *);
    void (*release)(struct ctl_catalog *,void *);
    void *ctx;
};
struct rec_catalog_scope {
    struct ctl_catalog *catalog;
    const struct ctl_owner_edit *owner; /* borrowed only inside original write */
    char target_record[CFG_BYTES+1]; /* exact candidate, also during preview */
    const char *field;
    const char *excluded; /* uninstall only: captured identity, no surviving policy */
    rec_resolve_prompt resolve;rec_model_ready ready;void *model_ctx;
    ctl_scoped_audit audit;void *audit_ctx;
    const struct rec_catalog_provider *provider;
    int checking,failed;
};
static int rec_catalog_source_current(const struct rec_catalog_scope *s) {
    return s->provider?s->provider->current(s->catalog,s->provider->ctx):ctl_catalog_current(s->catalog);
}
/* Sequential reuse: never retain one expanded table/schema per peer. Schema
 * is the original helper's private singleton. Restore it before returning to
 * target validation, on success AND every error. Its tokenizer pointers still
 * refer to the original singleton's arrays after the byte-for-byte restore. */
static int rec_catalog_current(void *ctx) {
    struct rec_catalog_scope *s=ctx;
    if(!s||!s->catalog||s->failed) return -1;
    if(s->checking) {s->failed=1;return -1;}
    s->checking=1;
    struct rec_catalog_work {
        struct ext_schema saved_schema;
        struct cfg defaults,effective;struct ctl_map map;
        char public[CFG_BYTES+1],view[CFG_BYTES+1];
    } *w=calloc(1,sizeof(*w));
    int rc=-1;
    if(!w) goto done;
    memcpy(&w->saved_schema,&schema,sizeof(schema));
    if(rec_catalog_source_current(s)) goto restore;
    for(size_t i=0;i<ctl_catalog_count(s->catalog);i++) {
        const char *name=ctl_catalog_name(s->catalog,i);
        if(name&&s->excluded&&!strcmp(name,s->excluded)) continue;
        const struct ctl_catalog_entry *e=&s->catalog->entries[i];
        const struct ctl_revision *r=name?e->revision:s->owner->revision;
        const char *record=name?e->record:s->target_record;
        if(!name) name=s->owner->name;
        size_t extent;
        int scripts=s->provider?s->provider->scripts(s->catalog,i,s->provider->ctx):s->catalog->scripts;
        int fd=ctl_revision_open(scripts,name,r);
        if(fd<0) goto restore;
        memset(&schema,0,sizeof(schema));
        int parsed=cfg_parse_script(r->bytes,r->size,&w->defaults,&extent);
        int ext=parsed?-1:rts_extensions(r->bytes,extent,&schema);close(fd);
        char declared_field[33];const char *field=cfg_value(&w->defaults,"LX06-RECOGNITION-FIELD");
        if(field){if(ats_policy_field(&w->defaults,&schema,declared_field))goto restore;field=declared_field;}
        else field=s->field; /* original legacy default projection, no model permission */
        if(ext<0||parsed||(r->size>=4&&!memcmp(r->bytes,"\177ELF",4))||
           /* The inherited editor still loads only its original prefix.
            * A late target declaration must not pass a complete audit while
            * the editor publishes a different partial cfg/view. Refuse that
            * unsupported target; do not silently replace its loaded object. */
           (r==s->owner->revision&&memcmp(&w->defaults,&s->owner->edit.defaults,sizeof(w->defaults)))||
           ctl_stored_map(record,&w->map)||ctl_effective(&w->defaults,&w->map,&w->effective)||
           udn_catalog_validate(&w->effective,&w->map)||
           rec_owner_project(&w->defaults,record,field,NULL,
               s->resolve,s->ready,s->model_ctx,w->view)||s->failed||
           rec_catalog_source_current(s)) goto restore;
    }
    rc=rec_catalog_source_current(s);
restore:
    memcpy(&schema,&w->saved_schema,sizeof(schema));
done:
    if(w) {rec_owner_wipe(w,sizeof(*w));free(w);}
    s->checking=0;
    if(rc) s->failed=1;
    return rc;
}
static int rec_catalog_audit(const char *public,void *scope,void *ctx) {
    struct rec_catalog_scope *s=ctx;
    if(!s||s->catalog||s->failed) return -1;
    const struct ctl_owner_edit *owner=scope;
    s->owner=owner;
    strcpy(s->target_record,owner->committed_record);
    /* The older catalog budgets captured peers. This complete-review entry
     * also counts the target retained by the original owner BEFORE any peer
     * allocation, without copying it or changing legacy callers' semantics. */
    s->catalog=s->provider?s->provider->acquire(scope,owner->revision->size,s->provider->ctx):
        ctl_catalog_acquire_impl(scope,owner->revision->size);
    if(!s->catalog||rec_catalog_current(s)||rdn_catalog_audit(public,scope,s)||
       rec_catalog_current(s)) {s->failed=1;return -1;}
    return 0;
}
/* Same dirty-patch/optional CTL/preview/load-revision semantics as the existing
 * entry. The uniform field is explicitly HOST-selected fixture data, never a
 * new product settings location. External model callbacks/gate remain stubs.
 * Peer full bytes + record content AND presence remain observed through the
 * native final guard, fsync, rename and uncertain return; no output on failure.
 * Caller callback must not retain scope or recursively start another writer. */
static int rec_catalog_apply_loaded_impl(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],const struct rec_catalog_provider *provider) {
    if(!audit||(provider&&(!provider->acquire||!provider->current||!provider->scripts||!provider->release))) return -1;
    struct rec_catalog_scope *s=calloc(1,sizeof(*s));
    if(!s) return -1;
    s->field=field;s->resolve=resolve;s->ready=ready;s->model_ctx=model_ctx;s->audit=audit;s->audit_ctx=audit_ctx;
    s->provider=provider;
    struct rec_owner_dependency dependency={rec_catalog_current,s};
    int rc=rec_owner_apply_loaded_impl(scripts,parent,entry,name,revision,field,app,
        changes,changes_length,ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,
        rec_catalog_audit,s,gate,gate_ctx,ctl_out,rec_out,&dependency);
    if(provider) provider->release(s->catalog,provider->ctx);
    else ctl_catalog_release(s->catalog);
    rec_owner_wipe(s,sizeof(*s));free(s);return rc;
}
int rec_catalog_apply_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1]) {
    return rec_catalog_apply_loaded_impl(scripts,parent,entry,name,revision,field,app,
        changes,changes_length,ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,
        audit,audit_ctx,gate,gate_ctx,ctl_out,rec_out,NULL);
}
