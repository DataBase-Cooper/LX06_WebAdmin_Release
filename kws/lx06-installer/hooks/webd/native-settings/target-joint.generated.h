static int rjt_locked(void *scope,void *ctx) {
    struct rec_unbind_work *w=ctx;
    struct ctl_owner_edit *o=&w->owner;
    struct ctl_writer_scope *writer=scope;
    w->scope=scope;o->store=ctl_writer_store(scope);o->lock=ctl_writer_lock(scope);o->store_root=writer->root;
    if(o->store<0||o->lock<0||ctl_store_root_current(o->revision->store_root,o->store)||
       ctl_owner_directory(o)) return -2;
    w->wake=wake_review_acquire(scope,w->wake_path,w->locks);
    if(!w->wake) return -1;
    /* No rec parse here. The original composed edit publishes the effective
     * object it already validates, including CTL overrides and restore. */
    int rc=w->forget?w->forget(w):
        cfg_store_guarded_locked(o->store,o->lock,o->name,rtp_owner_edit_record,ctl_owner_guard,o);
    w->receipt.settings_result=rc;
    if(rc<0) return o->refusal?o->refusal:rc;
    if(w->observe) return w->observe(w);
    w->receipt.required=w->wake->binding_present==1&&!strcmp(w->wake->bound,o->name)&&
        w->recognition.effective.official_mode==REC_OFF;
    if(w->transform.preview) return rc;
    struct ctl_revision *revision=o->compare_loaded;
    if(rc==1) {revision->record_ready=0;return 1;}
    /* Advance native target guard to the actual saved bytes. Peer selection,
     * wake snapshots, script and external gate remain pinned through clear. */
    int present=w->forget?0:strcmp(o->current,o->committed_record)?1:o->current_present;
    strcpy(o->current,o->committed_record);o->current_present=present;
    if(w->receipt.required) {
        w->receipt.attempted=1;
        w->committing=1;
        char path[128];snprintf(path,sizeof(path),"/proc/self/fd/%d/official_bind",w->wake->mutation->wake);
        w->receipt.binding_result=ru_original_write_at(path,"");
        w->committing=0;
        if(w->receipt.binding_result) {
            if(w->temp_fd>=0) {close(w->temp_fd);w->temp_fd=-1;}
            revision->record_ready=0;return 2;
        }
        /* Original writer now fsyncs its exclusive temp before guarded rename.
         * Once rename succeeded no old binding snapshot/owner callback can
         * reclassify it. Observe directory durability and the advanced wake
         * snapshot, retaining the inode through the final observation. */
        int sync_failed=fsync(w->wake->mutation->wake)!=0;
        int observation_failed=!w->renamed||w->observation_failed||wake_review_current(w->wake);
        if(w->temp_fd>=0) {close(w->temp_fd);w->temp_fd=-1;}
        if(sync_failed||observation_failed) {
            w->receipt.binding_result=1;revision->record_ready=0;return 1;
        }
    }
    if(w->forget) revision->record_ready=0; /* explicit reload after deletion */
    else {strcpy(revision->record,o->committed_record);revision->record_present=present;}
    return 0;
}
static int rjt_core(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *changes,size_t length,
        const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],struct rec_unbind_receipt *receipt,
        rec_unbind_prepare prepare,void *prepare_ctx,int (*forget)(struct rec_unbind_work *),
        int (*observe)(struct rec_unbind_work *),struct rtv_snapshot *view) {
    if(rec_unbind_active||(observe&&(!preview||forget))||parent<0||!revision||!revision->record_ready||!field||!ext_ident(field)||
       !strcmp(field,CTL_RECORD_KEY)||!name||!settings_name(name)||strcmp(name,revision->name)||
       !changes||!audit||!gate||!wake_path||!locks||!ctl_out||!rec_out||!receipt||
       (preview!=0&&preview!=1)||(restoring!=0&&restoring!=1)||(!ctl&&(ctl_length||restoring))||
       (provider&&(!provider->acquire||!provider->current||!provider->scripts||!provider->release))) return -1;
    struct rec_unbind_work *w=calloc(1,sizeof(*w));if(!w) return -1;
    w->temp_fd=-1;
    w->prepare=prepare;w->prepare_ctx=prepare_ctx;w->forget=forget;w->observe=observe;
    w->receipt=(struct rec_unbind_receipt){-1,0,0,-1,0};w->wake_path=wake_path;w->locks=locks;
    w->catalog=(struct rec_catalog_scope){.field=field,.resolve=resolve,.ready=ready,.model_ctx=model_ctx,
        .audit=audit,.audit_ctx=audit_ctx,.provider=provider};
    w->dependency=(struct rec_owner_dependency){rec_unbind_current,w};
    w->recognition=(struct rec_owner_work){.field=field,.changes=changes,.length=length,
        .resolve=resolve,.ready=ready,.ctx=model_ctx,.dependency=&w->dependency};
    w->transform=(struct ctl_owner_transform){rec_unbind_compose,w,preview,rec_unbind_composed_current};
    w->owner=(struct ctl_owner_edit){.revision=revision,.compare_loaded=revision,.scripts=scripts,
        .scripts_parent=parent,.scripts_entry=entry,.scripts_root=revision->scripts_root,.name=name,
        .full_view=view,.request=ctl,.request_size=ctl_length,.scoped_audit=rec_catalog_audit,.audit_ctx=&w->catalog,
        .gate=gate,.gate_ctx=gate_ctx,.transform=&w->transform};
    w->owner.edit.restoring=restoring;
    int result=-1;rec_unbind_active=w;
    int rc=ctl_writer_run(rjt_locked,w,gate,gate_ctx,&result);
    if(!rc) {
        rc=result;
        /* Publish views only after releases below, to keep uncertainty honest. */
        *receipt=w->receipt;
    }
    int release_failed=wake_review_release(w->wake)!=0;
    if(provider) provider->release(w->catalog.catalog,provider->ctx);
    else ctl_catalog_release(w->catalog.catalog);
    if(release_failed&&rc>=0) {receipt->release_failed=1;revision->record_ready=0;rc=1;}
    if(!rc) {strcpy(ctl_out,"{}"); /* complete public bytes belong to view */strcpy(rec_out,w->recognition.view);}
    rec_unbind_active=NULL;rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
static int rjt_read_locked(void *scope,void *ctx) {
    struct rec_readback_work *w=ctx;
    struct rec_unbind_work *op=&w->operation;
    int rc=rjt_locked(scope,op);
    if(rc) return rc;
    /* The original reader owns the grammar, including trailing whitespace.
     * EMPTY means present but parsed as unbound, not necessarily zero bytes. */
    w->observation=(struct rec_readback_observation){
        !op->wake->binding.present?REC_BIND_MISSING:
        !op->wake->binding_present?REC_BIND_EMPTY:
        !strcmp(op->wake->bound,op->owner.name)?REC_BIND_TARGET:REC_BIND_OTHER,
        op->recognition.effective.official_mode,op->wake->official_enabled,op->receipt.required};
    /* Include callback-caused changes and cancellation in the final snapshot.
     * All observed dependencies remain borrowed until both scopes release. */
    return ctl_writer_current(scope)||ctl_owner_guard(&op->owner)?-1:0;
}
/* Caller owns root/provider/callbacks for this synchronous call. *loaded must
 * be NULL: never discard the caller's old (possibly invalidated) revision.
 * On 0, transfer one fresh native revision plus both views and observation.
 * On every other return ALL caller outputs remain untouched. The host must
 * explicitly decide whether to adopt it and whether to issue a later save.
 * The initial loader releases its lock; preview then compares those complete
 * bytes under settings -> wake. An intervening edit refuses, never retries.
 * Non-reentrant/serialized host use, like the retained save entry. */
int recognition_target_joint_read(int scripts,int parent,const char *entry,const char *name,
        struct ctl_store_root *root,const char *field,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        struct rtv_snapshot **loaded,char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],
        struct rec_readback_observation *observation,size_t budget) {
    if(budget>GSV_BYTES||rec_unbind_active||scripts<0||parent<0||!entry||!root||!name||!settings_name(name)||
       !field||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||!audit||!gate||
       !wake_path||!locks||!loaded||*loaded||!ctl_out||!rec_out||!observation||
       (provider&&(!provider->acquire||!provider->current||!provider->scripts||!provider->release))) return -1;
    struct rec_readback_work *w=calloc(1,sizeof(*w));
    if(!w) return -1;
    struct rec_unbind_work *op=&w->operation;
    struct ctl_revision *revision=NULL;
    struct rtv_snapshot *view=NULL;
    rec_unbind_active=op;
    int rc=recognition_target_view_load(scripts,parent,entry,name,root,gate,gate_ctx,budget,&view);
    if(rc) goto done;
    revision=view->revision;view->revision=NULL;
    /* The guarded preview renders the exact loaded revision again.
     * Retire the initial public buffer before selecting this sink. */
    rtv_wipe(view->data,view->bytes+1);RTV_FREE(view->data,view->bytes+1);
    view->data=NULL;view->bytes=0;
    op->receipt=(struct rec_unbind_receipt){-1,0,0,-1,0};
    op->wake_path=wake_path;op->locks=locks;
    op->catalog=(struct rec_catalog_scope){.field=field,.resolve=resolve,.ready=ready,.model_ctx=model_ctx,
        .audit=audit,.audit_ctx=audit_ctx,.provider=provider};
    op->dependency=(struct rec_owner_dependency){rec_unbind_current,op};
    op->recognition=(struct rec_owner_work){.field=field,.changes="{}",.length=2,
        .resolve=resolve,.ready=ready,.ctx=model_ctx,.dependency=&op->dependency};
    op->transform=(struct ctl_owner_transform){rec_owner_compose,&op->recognition,1,rec_owner_guard};
    op->owner=(struct ctl_owner_edit){.revision=revision,.compare_loaded=revision,.scripts=scripts,
        .scripts_parent=parent,.scripts_entry=entry,.scripts_root=revision->scripts_root,.name=name,
        .full_view=view,.scoped_audit=rec_catalog_audit,.audit_ctx=&op->catalog,.gate=gate,.gate_ctx=gate_ctx,
        .transform=&op->transform};
    int result=-1;
    rc=ctl_writer_run(rjt_read_locked,w,gate,gate_ctx,&result);
    if(!rc) rc=result;
    /* Do not publish before release. A read-back is not a successful write,
     * even when it observes an empty binding after an uncertain fsync. */
done:
    if(wake_review_release(op->wake)&&!rc) rc=-1;
    if(provider) provider->release(op->catalog.catalog,provider->ctx);
    else ctl_catalog_release(op->catalog.catalog);
    if(!rc) {
        strcpy(ctl_out,"{}");strcpy(rec_out,op->recognition.view);
        *observation=w->observation;view->revision=revision;revision=NULL;
        *loaded=view;view=NULL;
    }
    recognition_target_view_close(view);
    ctl_owner_release(revision);
    rec_unbind_active=NULL;rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
