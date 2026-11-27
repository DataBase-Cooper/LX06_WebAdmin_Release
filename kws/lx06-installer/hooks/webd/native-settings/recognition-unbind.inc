/* A-05/A-06 HOST ONLY. Include after original helper, recognition owner/catalog,
 * writer, generated snapshots/readers/writer, and ctl-wake-review.inc.
 * One settings -> wake critical section, original save and original binding
 * writer. Two filesystem commits: NOT a crash-atomic multi-file transaction.
 * Save the non-executable official.off first. Never undo a committed save or
 * report a later unbind failure as "nothing changed". No automatic retry. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition unbind is fixture-only"
#endif
#ifndef LX06_REC_UNBIND_RECEIPT_DEFINED
#define LX06_REC_UNBIND_RECEIPT_DEFINED
struct rec_unbind_receipt {
    int settings_result,required,attempted,binding_result,release_failed;
};
#endif
/* Trusted synchronous input preparation, inside the SAME original transaction.
 * It may produce a candidate record, never write it or retain its arguments. */
typedef int (*rec_unbind_prepare)(const struct cfg *,const char *,char [CFG_BYTES+1],void *);
struct rec_unbind_work {
    struct ctl_owner_edit owner;
    struct rec_owner_work recognition;
    struct rec_catalog_scope catalog;
    struct rec_owner_dependency dependency;
    struct ctl_owner_transform transform;
    struct wake_review_source *wake;
    void *scope;
    const char *wake_path,*locks;
    struct rec_unbind_receipt receipt;
    struct stat temporary;
    int committing,temp_fd,renamed,observation_failed;
    rec_unbind_prepare prepare;
    void *prepare_ctx;
    /* Optional borrowed original whole-record forget, under these same locks.
     * Its successful candidate is ABSENT, not a saved empty object. */
    int (*forget)(struct rec_unbind_work *);
    /* Private synchronous completion after the SAME preview audit, before
     * either lock releases. Cache/runtime consumers retain their own meaning;
     * this never enters settings-delete or conditional-unbind logic. */
    int (*observe)(struct rec_unbind_work *);
};
/* Only the existing synchronous helper's rename hook borrows this pointer.
 * Nested calls are refused; host must serialize and keep callbacks alive. */
static struct rec_unbind_work *rec_unbind_active;
static int rec_unbind_compose(const struct cfg *defaults,const char *record,
        char next[CFG_BYTES+1],void *ctx) {
    struct rec_unbind_work *w=ctx;
    char *prepared=NULL;int rc=-1;
    if(w->prepare) {
        prepared=calloc(CFG_BYTES+1,1);
        if(!prepared||w->prepare(defaults,record,prepared,w->prepare_ctx)) goto done;
        record=prepared;
    }
    rc=rec_owner_compose(defaults,record,next,&w->recognition);
done:
    if(prepared) {rec_owner_wipe(prepared,CFG_BYTES+1);free(prepared);}
    return rc;
}
static int rec_unbind_composed_current(void *ctx) {
    struct rec_unbind_work *w=ctx;
    return rec_owner_guard(&w->recognition);
}
static int rec_unbind_current(void *ctx) {
    struct rec_unbind_work *w=ctx;
    return !w->wake||wake_review_current(w->wake)||rec_catalog_current(&w->catalog)?-1:0;
}
/* The SAME original writer's owned_temp hook. Hold its newly created inode
 * until the writer and its failure cleanup return: even empty replacements
 * and inode reuse cannot acquire ownership by matching a pathname. */
static int ru_temp_opened(int fd,const char *path) {
    struct rec_unbind_work *w=rec_unbind_active;
    char expected[132];struct stat st,named;
    if(!w||!w->committing||!w->wake||w->temp_fd>=0) return -1;
    snprintf(expected,sizeof(expected),"/proc/self/fd/%d/official_bind.tmp",w->wake->mutation->wake);
    if(strcmp(path,expected)||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size||
       fstatat(w->wake->mutation->wake,"official_bind.tmp",&named,AT_SYMLINK_NOFOLLOW)||
       !ctl_same_stat(&st,&named)) return -1;
    int held=fcntl(fd,F_DUPFD_CLOEXEC,3);
    if(held<0) {
        /* The original writer still owns fd here. Failure to duplicate must
         * not strand our exclusive empty temp and block every later request.
         * Recheck BOTH the live fd and name against the captured inode; a
         * replacement is never ours, even if it is another empty file. No
         * new fd, callback, retry or adoption is needed for this cleanup. */
        int saved=errno;
        if(!fstat(fd,&named)&&S_ISREG(named.st_mode)&&
           named.st_dev==st.st_dev&&named.st_ino==st.st_ino&&
           !fstatat(w->wake->mutation->wake,"official_bind.tmp",&named,AT_SYMLINK_NOFOLLOW)&&
           S_ISREG(named.st_mode)&&named.st_dev==st.st_dev&&named.st_ino==st.st_ino)
            (void)unlinkat(w->wake->mutation->wake,"official_bind.tmp",0);
        errno=saved;return -1;
    }
    w->temporary=st;w->temp_fd=held;return 0;
}
static int ru_temp_unlink(const char *path) {
    struct rec_unbind_work *w=rec_unbind_active;
    char expected[132];struct stat st,named;
    if(!w||!w->wake||w->temp_fd<0) {errno=ESTALE;return -1;}
    snprintf(expected,sizeof(expected),"/proc/self/fd/%d/official_bind.tmp",w->wake->mutation->wake);
    if(strcmp(path,expected)||fstat(w->temp_fd,&st)||!S_ISREG(st.st_mode)||
       fstatat(w->wake->mutation->wake,"official_bind.tmp",&named,AT_SYMLINK_NOFOLLOW)||
       !S_ISREG(named.st_mode)||st.st_dev!=named.st_dev||st.st_ino!=named.st_ino) {
        errno=ESTALE;return -1;
    }
    return unlinkat(w->wake->mutation->wake,"official_bind.tmp",0);
}
static int ru_guarded_rename(const char *from,const char *to) {
    struct rec_unbind_work *w=rec_unbind_active;
    char target[128],temporary[132];struct stat st,named;
    if(!w||!w->committing||!w->wake||w->temp_fd<0) {
        errno=ECANCELED;return -1;
    }
    snprintf(target,sizeof(target),"/proc/self/fd/%d/official_bind",w->wake->mutation->wake);
    snprintf(temporary,sizeof(temporary),"%s.tmp",target);
    /* External gate is allowed to call out: recheck the HELD inode and name
     * after it too. Never promote an arbitrary current empty file. */
    if(strcmp(from,temporary)||strcmp(to,target)||fstat(w->temp_fd,&st)||
       !ctl_same_stat(&w->temporary,&st)||st.st_size||st.st_nlink!=1||
       fstatat(w->wake->mutation->wake,"official_bind.tmp",&named,AT_SYMLINK_NOFOLLOW)||
       !ctl_same_stat(&st,&named)||ctl_writer_current(w->scope)||ctl_owner_guard(&w->owner)||
       fstat(w->temp_fd,&named)||!ctl_same_stat(&st,&named)||
       fstatat(w->wake->mutation->wake,"official_bind.tmp",&named,AT_SYMLINK_NOFOLLOW)||
       !ctl_same_stat(&st,&named)) {errno=ECANCELED;return -1;}
    int rc=rename(from,to);
    if(!rc) {
        w->renamed=1;
        /* rename changes ctime; only the held inode can advance the snapshot.
         * Failed observation is post-commit uncertainty, never no-write. */
        if(fstat(w->temp_fd,&st)||st.st_size||st.st_nlink!=1||
           st.st_mode!=w->temporary.st_mode||st.st_uid!=w->temporary.st_uid||st.st_gid!=w->temporary.st_gid||
           fstatat(w->wake->mutation->wake,"official_bind",&named,AT_SYMLINK_NOFOLLOW)||
           !ctl_same_stat(&st,&named)) w->observation_failed=1;
        else {
            wc_blob_release(&w->wake->binding);
            w->wake->binding=(struct wc_blob){.stat=st,.size=0,.bytes=NULL,.present=1};
            w->wake->binding_present=0;w->wake->bound[0]=0;
        }
    }
    return rc;
}
static int rec_unbind_locked(void *scope,void *ctx) {
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
        cfg_store_guarded_locked(o->store,o->lock,o->name,ctl_owner_edit_record,ctl_owner_guard,o);
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
/* Private result: 0 complete/preview; 1 durability uncertain; 2 settings
 * committed, unbind failed. Negative means refused before settings commit.
 * Receipt is published whenever the callback entered (even refusal); views
 * only on 0. Codes are NOT new HTTP/phone errors or a runtime authorization.
 * provider=NULL is explicitly the existing flat catalog, not web88 selection.
 * A native selector provider is borrowed through save/clear/release. */
static int rec_unbind_apply_core_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *changes,size_t length,
        const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],struct rec_unbind_receipt *receipt,
        rec_unbind_prepare prepare,void *prepare_ctx,int (*forget)(struct rec_unbind_work *),
        int (*observe)(struct rec_unbind_work *)) {
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
        .request=ctl,.request_size=ctl_length,.scoped_audit=rec_catalog_audit,.audit_ctx=&w->catalog,
        .gate=gate,.gate_ctx=gate_ctx,.transform=&w->transform};
    w->owner.edit.restoring=restoring;
    int result=-1;rec_unbind_active=w;
    int rc=ctl_writer_run(rec_unbind_locked,w,gate,gate_ctx,&result);
    if(!rc) {
        rc=result;
        /* Publish views only after releases below, to keep uncertainty honest. */
        *receipt=w->receipt;
    }
    int release_failed=wake_review_release(w->wake)!=0;
    if(provider) provider->release(w->catalog.catalog,provider->ctx);
    else ctl_catalog_release(w->catalog.catalog);
    if(release_failed&&rc>=0) {receipt->release_failed=1;revision->record_ready=0;rc=1;}
    if(!rc) {strcpy(ctl_out,w->owner.edit.public_json);strcpy(rec_out,w->recognition.view);}
    rec_unbind_active=NULL;rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
/* Existing operation consumers retain their exact signatures and behavior. */
static int rec_unbind_apply_operation_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *changes,size_t length,
        const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],struct rec_unbind_receipt *receipt,
        rec_unbind_prepare prepare,void *prepare_ctx,int (*forget)(struct rec_unbind_work *)) {
    return rec_unbind_apply_core_loaded(scripts,parent,entry,name,revision,field,changes,length,
        ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,
        wake_path,locks,provider,ctl_out,rec_out,receipt,prepare,prepare_ctx,forget,NULL);
}
/* Keep every existing prepared consumer's exact shape and save semantics. */
static int rec_unbind_apply_prepared_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *changes,size_t length,
        const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],struct rec_unbind_receipt *receipt,
        rec_unbind_prepare prepare,void *prepare_ctx) {
    return rec_unbind_apply_operation_loaded(scripts,parent,entry,name,revision,field,changes,length,
        ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,
        wake_path,locks,provider,ctl_out,rec_out,receipt,prepare,prepare_ctx,NULL);
}
/* Existing consumers keep their original call shape and omit preparation. */
int rec_unbind_apply_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *changes,size_t length,
        const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        const char *wake_path,const char *locks,const struct rec_catalog_provider *provider,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],struct rec_unbind_receipt *receipt) {
    return rec_unbind_apply_prepared_loaded(scripts,parent,entry,name,revision,field,changes,length,
        ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,
        wake_path,locks,provider,ctl_out,rec_out,receipt,NULL,NULL);
}
