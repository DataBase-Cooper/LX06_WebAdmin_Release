/* A-05 HOST ONLY: full declaration audit inside the ORIGINAL recognition
 * save/restore callback. Borrow its already selected catalog and candidate;
 * never capture another directory, use the old target record or open a writer.
 * Include after recognition-catalog/selection and the private audit seam. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) || !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "recognition declaration review is fixture-only"
#endif
typedef int (*rdn_session)(const char *,const char *,enum cfg_session_kind *,void *);
typedef int (*rdn_consume)(const char *,const struct cfg *,const struct decl_table *,
                         const char *,size_t,void *);
typedef int (*rdn_finish)(void *);
struct rdn_summary { size_t scripts,apps,channel_only,projections; };
struct rdn_work {
    struct rec_catalog_scope *scope;
    struct ctl_owner_edit *owner;
    int checking,running,used,completed,failed;
};
static struct rdn_work *rdn_active;
int recognition_declaration_current(void) {
    struct rdn_work *w=rdn_active;
    if(!w) return -1;
    if(w->checking) {w->failed=1;return -1;}
    w->checking=1;
    struct ctl_owner_edit *o=w->owner;
    int rc=w->failed||!o->scope_active||w->scope->owner!=o||
        ctl_owner_snapshot_current(o)||rec_catalog_current(w->scope)||
        (o->gate&&o->gate(o->gate_ctx))||cfg_store_lock_valid(o->store,o->lock)||
        ctl_owner_snapshot_current(o)||rec_catalog_source_current(w->scope);
    w->checking=0;
    if(rc||w->failed) {w->failed=1;return -1;}
    return 0;
}
int recognition_declaration_review(void *scope,const char *name,rdn_session session,
        rdn_consume consume,rdn_finish finish,void *ctx,struct rdn_summary *out) {
    struct rdn_work *w=rdn_active;
    if(!w) return -1;
    if(w->checking||w->running||w->used||scope!=w->owner||!name||
       strcmp(name,w->owner->name)||!session||!consume||!finish||!out) {
        w->failed=1;return -1;
    }
    w->used=w->running=1;
    struct rdn_projection {
        struct cfg defaults,effective;
        struct ctl_map map;
        struct cfg_bridge_app app;
        struct decl_table table;
        struct rdn_summary summary;
    } *p=NULL;
    int rc=-1;
    if(recognition_declaration_current()) goto done;
    p=calloc(1,sizeof(*p));if(!p) goto done;
    const struct ctl_catalog *catalog=w->scope->catalog;
    for(size_t i=0;i<catalog->count;i++) {
        const char *peer=ctl_catalog_name(catalog,i);
        const struct ctl_catalog_entry *entry=&catalog->entries[i];
        const struct ctl_revision *r=peer?entry->revision:w->owner->revision;
        const char *record=peer?entry->record:w->scope->target_record;
        if(!peer) peer=w->owner->name; /* SAME final candidate, not disk/defaults. */
        size_t extent=0;
        if(recognition_declaration_current()||!r||
           (r->size>=4&&!memcmp(r->bytes,"\177ELF",4))||
           cfg_parse_script(r->bytes,r->size,&p->defaults,&extent)||
           ctl_stored_map(record,&p->map)||ctl_effective(&p->defaults,&p->map,&p->effective)) goto done;
        if(!p->effective.app_count) {
            if(decl_compile_channels(&p->effective,&p->table)||
               consume(peer,&p->effective,&p->table,record,strlen(record),ctx)||
               recognition_declaration_current()) goto done;
            p->summary.channel_only++;p->summary.projections++;
        }
        for(size_t a=0;a<p->effective.app_count;a++) {
            const char *app=cfg_bridge_app_name(&p->effective,a);
            enum cfg_session_kind kind=(enum cfg_session_kind)-1;
            if(!app||session(peer,app,&kind,ctx)||recognition_declaration_current()||
               cfg_bridge_app_view(&p->effective,app,kind,&p->app)||
               decl_compile(&p->effective,app,&p->table)||
               consume(peer,&p->effective,&p->table,record,strlen(record),ctx)||
               recognition_declaration_current()) goto done;
            p->summary.apps++;p->summary.projections++;
        }
        p->summary.scripts++;
    }
    if(recognition_declaration_current()||finish(ctx)||recognition_declaration_current()) goto done;
    *out=p->summary;w->completed=1;rc=0;
done:
    if(p) {rec_owner_wipe(p,sizeof(*p));free(p);}
    if(rc) w->failed=1;
    w->running=0;return rc;
}
/* Private composition seam: original audit sees its unchanged public/scope/ctx.
 * Even a caller ignoring nested errors or omitting review cannot authorize it. */
static int rdn_catalog_audit(const char *public,void *owner,struct rec_catalog_scope *s) {
    if(rdn_active) {rdn_active->failed=1;return -1;}
    struct rdn_work w={.scope=s,.owner=owner};rdn_active=&w;
    int rc=-1;
    if(s&&owner&&s->owner==owner&&!recognition_declaration_current()&&
       !s->audit(public,owner,s->audit_ctx)&&w.completed&&!w.failed&&
       !recognition_declaration_current()) rc=0;
    rdn_active=NULL;return rc;
}
