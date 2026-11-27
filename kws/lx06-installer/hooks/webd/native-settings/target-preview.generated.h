static int rtp_edit_record(const char *current,char next[CFG_BYTES+1],void *opaque,struct rtv_snapshot *view) {
    struct ctl_edit *edit=opaque;
    struct ctl_map *old=calloc(1,sizeof(*old)),*merged=calloc(1,sizeof(*merged));
    struct cfg *effective=malloc(sizeof(*effective)); int rc=-1;
    if(!old||!merged||!effective||ctl_stored_map(current,old)) goto done;
    for(size_t i=0;i<old->count;i++) {
        int j=ctl_find(&edit->request,old->keys[i]);
        if(j>=0&&edit->restoring) continue;
        const char *v=j>=0?edit->request.arena+edit->request.offsets[j]:old->arena+old->offsets[i];
        if(ctl_add(merged,old->keys[i],v)) goto done;
    }
    if(!edit->restoring) for(size_t i=0;i<edit->request.count;i++)
        if(ctl_find(old,edit->request.keys[i])<0&&ctl_add(merged,edit->request.keys[i],edit->request.arena+edit->request.offsets[i])) goto done;
    if(ctl_effective(&edit->defaults,merged,effective)||rtv_render(effective,merged,view)||
       ctl_merge(current,NULL,1,merged,next)) goto done;
    rc=0;
done:
    free(old); free(merged); free(effective); return rc;
}
static int rtp_owner_edit_record(const char *current,char next[CFG_BYTES+1],void *ctx) {
    struct ctl_owner_edit *o=ctx;
    if(o->gate&&o->gate(o->gate_ctx)!=0) { o->refusal=-4; return -1; }
    strcpy(o->current,current);
    o->current_present=cfg_store_read(o->store,o->name,o->committed_record);
    if(o->current_present<0||strcmp(current,o->committed_record)) { o->refusal=-2; return -1; }
    if(o->compare_loaded&&(!o->compare_loaded->record_ready||
       o->compare_loaded->record_present!=o->current_present||
       strcmp(o->compare_loaded->record,current))) { o->refusal=-2; return -1; }
    if(ctl_owner_defaults(o)) return -1;
    if(o->request&&ctl_request_bytes(&o->edit,o->request,o->request_size)) return -1;
    /* Active App checks use CURRENT locked defaults, not a UI-supplied view. */
    if(!o->edit.restoring) for(size_t i=0;i<o->edit.request.count;i++)
        if(!ctl_active(&o->edit.defaults,o->edit.request.keys[i])) return -1;
    if(o->request) {
        if(rtp_edit_record(current,next,&o->edit,o->full_view)) return -1;
    } else {
        /* A trusted composition can omit CTL entirely. Do not manufacture an
         * empty old request (which the old parser deliberately rejects), or
         * rewrite/normalize any unchanged CTL tokens. */
        struct ctl_map *map=calloc(1,sizeof(*map));struct cfg *cfg=calloc(1,sizeof(*cfg));
        int invalid=!map||!cfg||ctl_stored_map(current,map)||ctl_effective(&o->edit.defaults,map,cfg)||
                    rtv_render(cfg,map,o->full_view);
        free(map);free(cfg);
        if(invalid) return -1;
        strcpy(next,current);
    }
    if(o->transform) {
        if(o->transform->apply(&o->edit.defaults,next,o->committed_record,o->transform->ctx)) {
            o->refusal=-3; return -1;
        }
        strcpy(next,o->committed_record);
    }
    o->scope_active=1;
    int audited=o->scoped_audit?o->scoped_audit(o->full_view->data,o,o->audit_ctx):
                               o->audit(o->full_view->data,o->audit_ctx);
    o->scope_active=0;
    if(audited!=0) { o->refusal=-3; return -1; }
    if(o->declarations) {
        struct ctl_map map; struct cfg candidate;
        /* Reuse the actual merged record, including current user overrides;
         * never parse a second header or trust a UI's old declaration report. */
        if(ctl_stored_map(next,&map)||ctl_effective(&o->edit.defaults,&map,&candidate)||
           o->declarations(&candidate,o->declarations_ctx)!=0) {
            o->refusal=-3; return -1;
        }
    }
    /* A preview takes the same lock/audits/guard but never renames or advances
     * the loaded bytes to an uncommitted candidate. Output remains a preview. */
    if(o->transform&&o->transform->preview) strcpy(next,current);
    if(o->compare_loaded) strcpy(o->committed_record,next);
    return 0;
}
static int rtp_owner_apply(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,
                    ctl_scoped_audit scoped_audit,int parent,const char *entry,
                    struct ctl_revision *compare_loaded,const struct ctl_owner_transform *transform,
                    char out[CFG_BYTES+1],struct rtv_snapshot *view) {
    if(!revision||(!request&&(!transform||length||restoring))||!out||(!audit&&!scoped_audit)||!name||!settings_name(name)||
       strcmp(name,revision->name)||(restoring!=0&&restoring!=1)||!ctl_script_dir(scripts)) return -1;
    if(parent>=0&&ctl_directory_current(scripts,parent,entry)) return -2;
    struct ctl_owner_edit *o=calloc(1,sizeof(*o));
    if(!o) return -1;
    o->full_view=view;
    o->scripts=scripts; o->scripts_parent=parent; o->scripts_entry=entry;
    o->name=name; o->revision=revision; o->audit=audit; o->audit_ctx=audit_ctx;
    o->compare_loaded=compare_loaded;
    o->transform=transform;
    o->scoped_audit=scoped_audit;
    o->gate=gate; o->gate_ctx=gate_ctx;
    o->declarations=declarations; o->declarations_ctx=declarations_ctx;
    o->request=request; o->request_size=length;
    o->edit.restoring=restoring; o->store=cfg_store_open(0);
    o->store_root=revision->store_root; /* borrowed until synchronous return */
    o->scripts_root=revision->scripts_root;
    int rc=-1,lock=-1;
    if(ctl_owner_directory(o)) { rc=-2; goto done; }
    lock=openat(o->store,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    o->lock=lock;
    /* Existing transaction validates lock identity/mode and acquires it. */
    if(lock<0) goto done;
    rc=cfg_store_guarded_locked(o->store,lock,name,rtp_owner_edit_record,ctl_owner_guard,o);
    if(compare_loaded) {
        if(!rc) {
            strcpy(compare_loaded->record,o->committed_record);
            /* A no-op does not materialize a missing record. */
            compare_loaded->record_present=strcmp(o->current,o->committed_record)?1:o->current_present;
        } else if(rc==1) compare_loaded->record_ready=0;
    }
    if(!rc) strcpy(out,"{}"); /* full candidate is owned by view */
done:
    if(lock>=0) close(lock);
    if(o->store>=0) close(o->store);
    if(rc<0&&o->refusal) rc=o->refusal;
    free(o); return rc;
}
static int rtp_rec_apply(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],const struct rec_owner_dependency *dependency,struct rtv_snapshot *view) {
    if(parent<0||!revision||!field||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||
       (app&&!ext_ident(app))||!changes||!rec_out||!ctl_out||(preview!=0&&preview!=1)||
       (dependency&&!dependency->current)) return -1;
    struct rec_owner_work *w=calloc(1,sizeof(*w));char *public=calloc(CFG_BYTES+1,1);
    int rc=-1;
    if(!w||!public) goto done;
    w->field=field;w->app=app;w->changes=changes;w->length=changes_length;
    w->resolve=resolve;w->ready=ready;w->ctx=model_ctx;
    w->dependency=dependency;
    struct ctl_owner_transform transform={rec_owner_compose,w,preview,rec_owner_guard};
    rc=rtp_owner_apply(scripts,name,revision,ctl,ctl_length,restoring,NULL,audit_ctx,
        gate,gate_ctx,NULL,NULL,audit,parent,entry,revision,&transform,public,view);
    if(!rc) {strcpy(ctl_out,public);strcpy(rec_out,w->view);}
done:
    if(w) {rec_owner_wipe(w,sizeof(*w));free(w);}
    if(public) {rec_owner_wipe(public,CFG_BYTES+1);free(public);}return rc;
}
static int rtp_catalog_apply(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],const struct rec_catalog_provider *provider,struct rtv_snapshot *view) {
    if(!audit||(provider&&(!provider->acquire||!provider->current||!provider->scripts||!provider->release))) return -1;
    struct rec_catalog_scope *s=calloc(1,sizeof(*s));
    if(!s) return -1;
    s->field=field;s->resolve=resolve;s->ready=ready;s->model_ctx=model_ctx;s->audit=audit;s->audit_ctx=audit_ctx;
    s->provider=provider;
    struct rec_owner_dependency dependency={rec_catalog_current,s};
    int rc=rtp_rec_apply(scripts,parent,entry,name,revision,field,app,
        changes,changes_length,ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,
        rec_catalog_audit,s,gate,gate_ctx,ctl_out,rec_out,&dependency,view);
    if(provider) provider->release(s->catalog,provider->ctx);
    else ctl_catalog_release(s->catalog);
    rec_owner_wipe(s,sizeof(*s));free(s);return rc;
}
int recognition_target_preview(int scripts,int parent,const char *entry,const char *name,
        struct rtv_snapshot *loaded,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],
        const struct ctl_selection_root roots[3],struct ctl_store_root *const ancestors[3],size_t budget,struct rtv_snapshot **out) {
    if(!roots||!ancestors||!loaded||!loaded->revision||!out||*out||!gate||
       preview!=1||budget>GSV_BYTES) return -1;
    struct rtv_snapshot *view=calloc(1,sizeof(*view));
    if(!view) return -1;
    view->budget=budget;
    struct rec_selection_scope s={roots,ancestors,NULL};
    const struct rec_catalog_provider provider={rec_selection_acquire,rec_selection_current,
        rec_selection_scripts,rec_selection_release,&s};
    int rc=rtp_catalog_apply(scripts,parent,entry,name,loaded->revision,field,app,
        changes,changes_length,ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,
        audit,audit_ctx,gate,gate_ctx,ctl_out,rec_out,&provider,view);
    if(!rc) *out=view;else recognition_target_view_close(view);
    return rc;
}
