/* A-05/A-06/A-11 HOST ONLY. Original joint operation, complete target sink. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "full target joint settings are fixture-only"
#endif
/* A30: dirty connection inputs join the ORIGINAL locked transaction. Reuse
 * ext_value and rec_owner_member; no defaults, second save or public secrets.
 * ctl_legacy_edit requires a COMPLETE schema projection, not a dirty patch. */
static int rjt_connection(const struct cfg *cfg,const char *record,char next[CFG_BYTES+1],void *ctx) {
    (void)cfg;
    const char *raw=ctx;xz_json_doc d;
    if(!raw||strlen(raw)>EXT_CAP||xz_json_parse(&d,raw,strlen(raw))||
       !ext_keys(&d,0,"|host||port||ca_file||spki||token||phone_allowlist||speaker_allowlist|")) return -1;
    char merged[CFG_BYTES+1],quoted[CFG_BYTES+1],key[33],value[2049];int rc=-1;
    strcpy(next,record);
    for(int k=d.tokens[0].first_child;k>=0;) {
        int v=d.tokens[k].next_sibling;
        if(v<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
           !ext_text(&d,0,key,value,sizeof(value),0)) goto done;
        int f=ext_field_index(&schema,key);
        if(f<0||!ext_value(&schema,f,value)||
           (!strcmp(key,"token")&&strcmp(schema.fields[f].type,"password"))) goto done;
        /* Blank password is a placeholder, never an erase operation. */
        if(!strcmp(key,"token")&&*value&&strcmp(value,"@@TOKEN@@")&&
           (strlen(value)<32||strlen(value)>128||strspn(value,
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")!=strlen(value))) goto done;
        if(strcmp(key,"token")||(*value&&strcmp(value,"@@TOKEN@@"))) {
            struct ctl_buf b={quoted,0};const char *path[]={key};
            if(ctl_quote(&b,value)||rec_owner_member(next,path,1,quoted,merged)) goto done;
            strcpy(next,merged);
        }
        rec_owner_wipe(value,sizeof(value));k=d.tokens[v].next_sibling;
    }
    rc=0;
done:
    rec_owner_wipe(value,sizeof(value));rec_owner_wipe(merged,sizeof(merged));
    rec_owner_wipe(quoted,sizeof(quoted));return rc;
}
static int rjt_apply_values(int scripts,int parent,const char *entry,const char *name,
        struct rtv_snapshot *loaded,const char *field,const char *app,
        const char *changes,size_t length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],
        const struct ctl_selection_root roots[3],struct ctl_store_root *const ancestors[3],
        size_t budget,struct rtv_snapshot **out,const char *wake_path,const char *locks,
        struct rec_unbind_receipt *receipt,const char *connection) {
    if(!loaded||!loaded->revision||!loaded->revision->record_ready||!out||*out||
       !roots||!ancestors||!receipt||!gate||app||budget>GSV_BYTES||
       (preview!=0&&preview!=1)) return -1;
    struct rtv_snapshot *view=calloc(1,sizeof(*view));
    if(!view) {if(!preview) loaded->revision->record_ready=0;return -1;}
    view->budget=budget;
    struct rec_selection_scope s={roots,ancestors,NULL};
    const struct rec_catalog_provider provider={rec_selection_acquire,rec_selection_current,
        rec_selection_scripts,rec_selection_release,&s};
    int rc=rjt_core(scripts,parent,entry,name,loaded->revision,field,changes,length,
        ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,
        wake_path,locks,&provider,ctl_out,rec_out,receipt,
        connection?rjt_connection:NULL,(void *)connection,NULL,NULL,view);
    if(!preview) {
        loaded->revision->record_ready=0;
        loaded->revision->profile_ready=rc==0;
    }
    if(!rc) *out=view;else recognition_target_view_close(view);
    return rc;
}
/* Keep the original ABI for every existing complete-form consumer. */
#define RJT_ARGS int scripts,int parent,const char *entry,const char *name, \
        struct rtv_snapshot *loaded,const char *field,const char *app, \
        const char *changes,size_t length,const char *ctl,size_t ctl_length,int restoring,int preview, \
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx, \
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx, \
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1], \
        const struct ctl_selection_root roots[3],struct ctl_store_root *const ancestors[3], \
        size_t budget,struct rtv_snapshot **out,const char *wake_path,const char *locks, \
        struct rec_unbind_receipt *receipt
#define RJT_PASS scripts,parent,entry,name,loaded,field,app,changes,length,ctl,ctl_length,restoring,preview, \
        resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,ctl_out,rec_out,roots,ancestors,budget,out,wake_path,locks,receipt
int recognition_target_joint_apply(RJT_ARGS) {return rjt_apply_values(RJT_PASS,NULL);}
int recognition_target_joint_apply_values(RJT_ARGS,const char *connection) {
    if(!connection) return -1;
    return rjt_apply_values(RJT_PASS,connection);
}
#undef RJT_ARGS
#undef RJT_PASS
int recognition_target_joint_read_selected(int scripts,int parent,const char *entry,const char *name,
        struct ctl_store_root *root,const char *field,rec_resolve_prompt resolve,rec_model_ready ready,
        ctl_scoped_audit audit,ctl_owner_gate gate,const char *wake_path,const char *locks,
        const struct ctl_selection_root roots[3],struct ctl_store_root *const ancestors[3],
        struct rtv_snapshot **out,struct rec_readback_observation *observation,size_t budget) {
    if(!roots||!ancestors) return -1;
    struct rec_selection_scope s={roots,ancestors,NULL};
    const struct rec_catalog_provider provider={rec_selection_acquire,rec_selection_current,
        rec_selection_scripts,rec_selection_release,&s};
    char a[CFG_BYTES+1],b[CFG_BYTES+1];
    return recognition_target_joint_read(scripts,parent,entry,name,root,field,resolve,ready,NULL,
        audit,NULL,gate,NULL,wake_path,locks,&provider,out,a,b,observation,budget);
}
