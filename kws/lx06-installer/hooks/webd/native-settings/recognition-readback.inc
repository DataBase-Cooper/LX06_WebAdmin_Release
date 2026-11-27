/* A-05/A-06 HOST ONLY. Include after recognition-unbind.inc.
 * Explicit observation after a partial save or a lost response. Reuse the
 * original rooted loader and the exact unbind preview/audit critical section.
 * No writes, repair, replay, durability inference or runtime authorization.
 * A successful read cannot prove which earlier request wrote these bytes. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition readback is fixture-only"
#endif
enum rec_readback_binding { REC_BIND_MISSING, REC_BIND_EMPTY, REC_BIND_TARGET, REC_BIND_OTHER };
struct rec_readback_observation {
    int binding,official_mode,official_kws_enabled,unbind_required;
};
struct rec_readback_work {
    struct rec_unbind_work operation;
    struct rec_readback_observation observation;
};
static int rec_readback_locked(void *scope,void *ctx) {
    struct rec_readback_work *w=ctx;
    struct rec_unbind_work *op=&w->operation;
    int rc=rec_unbind_locked(scope,op);
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
int rec_readback_rooted(int scripts,int parent,const char *entry,const char *name,
        struct ctl_store_root *root,const char *field,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        struct ctl_revision **loaded,char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],
        struct rec_readback_observation *observation) {
    if(rec_unbind_active||scripts<0||parent<0||!entry||!root||!name||!settings_name(name)||
       !field||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||!audit||!gate||
       !wake_path||!locks||!loaded||*loaded||!ctl_out||!rec_out||!observation||
       (provider&&(!provider->acquire||!provider->current||!provider->scripts||!provider->release))) return -1;
    struct rec_readback_work *w=calloc(1,sizeof(*w));
    if(!w) return -1;
    struct rec_unbind_work *op=&w->operation;
    struct ctl_revision *revision=NULL;
    rec_unbind_active=op;
    int rc=ctl_owner_load_guarded_rooted(scripts,parent,entry,name,&revision,root,
        gate,gate_ctx,op->owner.edit.public_json);
    if(rc) goto done;
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
        .scoped_audit=rec_catalog_audit,.audit_ctx=&op->catalog,.gate=gate,.gate_ctx=gate_ctx,
        .transform=&op->transform};
    int result=-1;
    rc=ctl_writer_run(rec_readback_locked,w,gate,gate_ctx,&result);
    if(!rc) rc=result;
    /* Do not publish before release. A read-back is not a successful write,
     * even when it observes an empty binding after an uncertain fsync. */
done:
    if(wake_review_release(op->wake)&&!rc) rc=-1;
    if(provider) provider->release(op->catalog.catalog,provider->ctx);
    else ctl_catalog_release(op->catalog.catalog);
    if(!rc) {
        strcpy(ctl_out,op->owner.edit.public_json);strcpy(rec_out,op->recognition.view);
        *observation=w->observation;*loaded=revision;revision=NULL;
    }
    ctl_owner_release(revision);
    rec_unbind_active=NULL;rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
