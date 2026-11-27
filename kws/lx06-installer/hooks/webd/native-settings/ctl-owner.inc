/* T24 fixture-only, included AFTER the privately patched original helper.
 * No CLI/HTTP/IPC registration. The synchronous host injects the existing
 * matcher/K3 audit through a function pointer; no duplicate validator here.
 * All settings writers AND script upload/delete owners must use the existing
 * settings .lock. Guards detect observed uncooperative changes but do not
 * replace that upload-owner integration requirement. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T24 owner adapter is fixture-only"
#endif
#define CTL_SOURCE_SCRIPT_CAP (1024u*1024u)
#include "ctl-store-root.inc"
typedef int (*ctl_candidate_audit)(const char *public_candidate,void *ctx);
typedef int (*ctl_scoped_audit)(const char *public_candidate,void *borrowed_scope,void *ctx);
typedef int (*ctl_owner_gate)(void *ctx);
typedef int (*ctl_declaration_audit)(const struct cfg *candidate,void *ctx);
/* Trusted, synchronous composition only. A transform reuses the current
 * locked defaults/record; it must not retain them or start another writer. */
struct ctl_owner_transform {
    int (*apply)(const struct cfg *,const char *,char [CFG_BYTES+1],void *);
    void *ctx;
    int preview;
    int (*guard)(void *);
};
struct ctl_revision {
    SETTINGS_STAT stat;
    size_t size;
    char *bytes;
    char name[65];
    struct ctl_store_root *store_root;
    struct ctl_store_root *scripts_root;
    /* Private loaded settings snapshot; never part of the public view.
     * Presence distinguishes missing from an explicitly stored empty object. */
    char *record; /* allocated only for a loaded editor, not catalog entries */
    int record_present, record_ready;
    int profile_ready, profile_borrowed; /* one confirmed save, never a load/preview */
};
struct ctl_owner_edit {
    struct ctl_edit edit;
    struct rtv_snapshot *full_view;
    const struct ctl_revision *revision;
    int scripts, store, refusal, lock, scope_active, scripts_parent;
    const char *scripts_entry;
    struct ctl_store_root *store_root;
    struct ctl_store_root *scripts_root;
    const char *name;
    const char *request;
    size_t request_size;
    char current[CFG_BYTES+1];
    int current_present;
    struct ctl_revision *compare_loaded;
    char committed_record[CFG_BYTES+1];
    ctl_candidate_audit audit;
    ctl_scoped_audit scoped_audit;
    void *audit_ctx;
    ctl_owner_gate gate;
    void *gate_ctx;
    ctl_declaration_audit declarations;
    void *declarations_ctx;
    const struct ctl_owner_transform *transform;
};
/* Private fixture return codes: 0 durable, 1 uncertain durability;
 * -1 unavailable/invalid, -2 stale script/dependency, -3 audit refused,
 * -4 dependency/cancellation gate refused before commit.
 * None of these is a new product, phone or transport error. */
static int ctl_script_dir(int dir) {
    SETTINGS_STAT st;
    return dir>=0&&!SETTINGS_FSTAT(dir,&st)&&S_ISDIR(st.st_mode)&&
           st.st_uid==getuid()&&!(st.st_mode&0022);
}
/* An open directory alone survives rename/unlink. Pin its immediate named
 * entry to an owner-held parent fd; never follow a replacement symlink.
 * This is not a claim to pin every ancestor of an absolute pathname. */
static int ctl_directory_current(int scripts,int parent,const char *entry) {
    SETTINGS_STAT opened,parent_stat; struct stat named;
    if(!entry||!*entry||strlen(entry)>255||strchr(entry,'/')||
       !strcmp(entry,".")||!strcmp(entry,"..")||parent<0||SETTINGS_FSTAT(parent,&parent_stat)||
       !S_ISDIR(parent_stat.st_mode)||
       /* An owner-controlled parent or trusted sticky TMP parent. */
       !((parent_stat.st_uid==getuid()&&!(parent_stat.st_mode&0022))||
         ((parent_stat.st_uid==0||parent_stat.st_uid==getuid())&&(parent_stat.st_mode&S_ISVTX)))||
       !ctl_script_dir(scripts)||SETTINGS_FSTAT(scripts,&opened)||
       fstatat(parent,entry,&named,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(named.st_mode)) return -1;
    return opened.st_dev==named.st_dev&&opened.st_ino==named.st_ino&&
        opened.st_uid==named.st_uid&&opened.st_mode==named.st_mode&&
        opened.st_nlink==named.st_nlink?0:-1;
}
static int ctl_owner_directory(struct ctl_owner_edit *o) {
    if(ctl_store_root_current(o->store_root,o->store)||
       (o->scripts_root&&ctl_store_root_current(o->scripts_root,o->scripts))) {
        o->refusal=-2; return -1;
    }
    if(o->scripts_parent>=0&&ctl_directory_current(o->scripts,o->scripts_parent,o->scripts_entry)) {
        o->refusal=-2; return -1;
    }
    return 0;
}
static int ctl_same_stat(const SETTINGS_STAT *a,const SETTINGS_STAT *b) {
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&
        a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_nlink==b->st_nlink&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int ctl_script_open(int dir,const char *name,SETTINGS_STAT *st) {
    if(!ctl_script_dir(dir)||!name||!settings_name(name)) return -1;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) return -1;
    if(SETTINGS_FSTAT(fd,st)||!S_ISREG(st->st_mode)||st->st_uid!=getuid()||
       st->st_nlink!=1||(st->st_mode&0022)||st->st_size<0||
       (uintmax_t)st->st_size>CTL_SOURCE_SCRIPT_CAP) { close(fd); return -1; }
    return fd;
}
static int ctl_pread_exact(int fd,char *out,size_t size,off_t offset) {
    size_t got=0;
    while(got<size) {
        ssize_t n=pread(fd,out+got,size-got,offset+(off_t)got);
        if(n<0&&errno==EINTR) continue;
        if(n<=0) return -1;
        got+=(size_t)n;
    }
    return 0;
}
void ctl_owner_release(struct ctl_revision *revision) {
    if(!revision) return;
    ctl_store_root_release(revision->store_root);
    ctl_store_root_release(revision->scripts_root);
    if(revision->record) {
        volatile char *p=revision->record;
        for(size_t i=0;i<CFG_BYTES+1;i++) p[i]=0;
        free(revision->record);
    }
    /* Revision includes a private script snapshot; never return it to the UI. */
    if(revision->bytes) {
        volatile char *p=revision->bytes;
        for(size_t i=0;i<revision->size;i++) p[i]=0;
        free(revision->bytes);
    }
    volatile unsigned char *private_bytes=(volatile unsigned char *)revision;
    for(size_t i=0;i<sizeof(*revision);i++) private_bytes[i]=0;
    free(revision);
}
static struct ctl_revision *ctl_capture(int dir,const char *name) {
    struct ctl_revision *r=calloc(1,sizeof(*r)); SETTINGS_STAT after;
    if(!r) return NULL;
    int fd=ctl_script_open(dir,name,&r->stat);
    if(fd<0) goto bad;
    r->size=(size_t)r->stat.st_size;
    r->bytes=calloc(r->size+1,1); strcpy(r->name,name);
    if(!r->bytes||ctl_pread_exact(fd,r->bytes,r->size,0)||SETTINGS_FSTAT(fd,&after)||
       !ctl_same_stat(&r->stat,&after)) { close(fd); goto bad; }
    close(fd); return r;
bad:
    ctl_owner_release(r); return NULL;
}
/* Check the ENTIRE script, including executable bytes past the header budget.
 * Same bytes at a new inode are still a reupload and require a fresh load. */
static int ctl_revision_open(int dir,const char *name,const struct ctl_revision *r) {
    SETTINGS_STAT st,after; char block[1024];
    if(!r||strcmp(name,r->name)) return -1;
    int fd=ctl_script_open(dir,name,&st);
    if(fd<0) return -1;
    if(!ctl_same_stat(&r->stat,&st)) goto bad;
    for(size_t i=0;i<r->size;) {
        size_t n=r->size-i; if(n>sizeof(block)) n=sizeof(block);
        if(ctl_pread_exact(fd,block,n,(off_t)i)||memcmp(block,r->bytes+i,n)) goto bad;
        i+=n;
    }
    if(SETTINGS_FSTAT(fd,&after)||!ctl_same_stat(&st,&after)) goto bad;
    return fd;
bad:
    close(fd); return -1;
}
#include "recognition-target-source.inc"
static int ctl_owner_defaults(struct ctl_owner_edit *o) {
    if(ctl_owner_directory(o)) return -1;
    int fd=ctl_revision_open(o->scripts,o->name,o->revision);
    if(fd<0) { o->refusal=-2; return -1; }
    /* The original helper starts with an empty process singleton. Retained
     * host calls must do the same: ext_read returning 0 is no declaration,
     * never permission to reuse the previous target's schema/secret defaults. */
    memset(&schema,0,sizeof(schema));
    size_t extent=0;
    int rc=rts_defaults(o->revision,&o->edit.defaults,&extent);
    int ext=rc?-1:rts_extensions(o->revision->bytes,extent,&schema);
    close(fd);
    return rc||ext<0||(ext==1&&ext_field_index(&schema,CTL_RECORD_KEY)>=0)?-1:0;
}
static int ctl_owner_snapshot_current(struct ctl_owner_edit *o) {
    char current[CFG_BYTES+1];
    if(ctl_owner_directory(o)) return -1;
    int fd=ctl_revision_open(o->scripts,o->name,o->revision);
    if(fd<0) { o->refusal=-2; return -1; }
    close(fd);
    int present=cfg_store_read(o->store,o->name,current);
    if(present<0||present!=o->current_present||strcmp(current,o->current)) {
        o->refusal=-2; return -1;
    }
    return 0;
}
static int ctl_owner_guard(void *ctx) {
    struct ctl_owner_edit *o=ctx;
    if(ctl_owner_snapshot_current(o)) return -1;
    if(o->gate&&o->gate(o->gate_ctx)!=0) { o->refusal=-4; return -1; }
    if(o->transform&&o->transform->guard&&o->transform->guard(o->transform->ctx)) {
        o->refusal=-3; return -1;
    }
    /* A synchronous gate can change the record, script OR lock after the
     * first check. Recheck the whole snapshot before rename/no-op success. */
    if(cfg_store_lock_valid(o->store,o->lock)) { o->refusal=-2; return -1; }
    return ctl_owner_snapshot_current(o);
}
static int ctl_owner_edit_record(const char *current,char next[CFG_BYTES+1],void *ctx) {
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
        if(ctl_edit_record(current,next,&o->edit)) return -1;
    } else {
        /* A trusted composition can omit CTL entirely. Do not manufacture an
         * empty old request (which the old parser deliberately rejects), or
         * rewrite/normalize any unchanged CTL tokens. */
        struct ctl_map *map=calloc(1,sizeof(*map));struct cfg *cfg=calloc(1,sizeof(*cfg));
        int invalid=!map||!cfg||ctl_stored_map(current,map)||ctl_effective(&o->edit.defaults,map,cfg)||
                    ctl_public(cfg,map,o->edit.public_json);
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
    int audited=o->scoped_audit?o->scoped_audit(o->edit.public_json,o,o->audit_ctx):
                               o->audit(o->edit.public_json,o->audit_ctx);
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
static int ctl_owner_load_record(const char *current,char next[CFG_BYTES+1],void *ctx) {
    struct ctl_owner_edit *o=ctx; struct ctl_map map; struct cfg effective;
    if(o->gate&&o->gate(o->gate_ctx)!=0) { o->refusal=-4; return -1; }
    strcpy(o->current,current);
    o->current_present=cfg_store_read(o->store,o->name,o->committed_record);
    if(o->current_present<0||strcmp(current,o->committed_record)) { o->refusal=-2; return -1; }
    o->revision=ctl_capture(o->scripts,o->name);
    if(o->revision) {
        struct ctl_revision *loaded=(struct ctl_revision *)o->revision;
        loaded->record=calloc(CFG_BYTES+1,1);
        if(!loaded->record) return -1;
        strcpy(loaded->record,current);
        loaded->record_present=o->current_present;
        loaded->record_ready=1;
        ((struct ctl_revision *)o->revision)->store_root=ctl_store_root_retain(o->store_root);
        ((struct ctl_revision *)o->revision)->scripts_root=ctl_store_root_retain(o->scripts_root);
    }
    if(!o->revision||ctl_owner_defaults(o)||ctl_stored_map(current,&map)||
       ctl_effective(&o->edit.defaults,&map,&effective)||
       (o->full_view?rtv_render(&effective,&map,o->full_view):ctl_public(&effective,&map,o->edit.public_json))) return -1;
    strcpy(next,current); return 0;
}
static int ctl_owner_load_guard(void *ctx) {
    struct ctl_owner_edit *o=ctx;
    /* A synchronous dependency can change the script, record, lock or roots.
     * Check the complete snapshot AFTER it returns, while the original lock
     * is still held. Never publish a candidate captured before that change. */
    if(o->gate&&o->gate(o->gate_ctx)!=0) { o->refusal=-4; return -1; }
    return cfg_store_lock_valid(o->store,o->lock)||ctl_owner_snapshot_current(o);
}
static int ctl_owner_load_impl(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_revision **revision,struct ctl_store_root *root,
                   ctl_owner_gate gate,void *gate_ctx,char out[CFG_BYTES+1]) {
    if(!revision||!out||!name||!settings_name(name)||!ctl_script_dir(scripts)) return -1;
    if(gate&&gate(gate_ctx)!=0) return -4;
    if(parent>=0&&ctl_directory_current(scripts,parent,entry)) return -2;
    if(root&&ctl_store_root_current(root,scripts)) return -2;
    struct ctl_owner_edit *o=calloc(1,sizeof(*o));
    if(!o) return -1;
    o->scripts=scripts; o->scripts_parent=parent; o->scripts_entry=entry;
    o->gate=gate; o->gate_ctx=gate_ctx;
    o->scripts_root=root; /* borrowed through synchronous load */
    o->name=name; o->store=cfg_store_open(0);
    o->store_root=o->store<0?NULL:ctl_store_root_open(o->store);
    int lock=!o->store_root?-1:openat(o->store,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    o->lock=lock;
    int rc=lock<0?-1:cfg_store_guarded_locked(o->store,lock,name,ctl_owner_load_record,
                                                          gate?ctl_owner_load_guard:ctl_owner_guard,o);
    if(!rc) { memcpy(out,o->edit.public_json,strlen(o->edit.public_json)+1);
        *revision=(struct ctl_revision *)o->revision; }
    else ctl_owner_release((struct ctl_revision *)o->revision);
    if(rc&&gate&&o->refusal) rc=o->refusal;
    if(lock>=0) close(lock);
    if(o->store>=0) close(o->store);
    ctl_store_root_release(o->store_root);
    free(o); return rc;
}
int ctl_owner_load_at(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_revision **revision,char out[CFG_BYTES+1]) {
    if(parent<0) return -1;
    return ctl_owner_load_impl(scripts,parent,entry,name,revision,NULL,NULL,NULL,out);
}
int ctl_owner_load(int scripts,const char *name,struct ctl_revision **revision,
                   char out[CFG_BYTES+1]) {
    return ctl_owner_load_impl(scripts,-1,NULL,name,revision,NULL,NULL,NULL,out);
}
int ctl_owner_load_rooted(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_revision **revision,struct ctl_store_root *root,char out[CFG_BYTES+1]) {
    if(parent<0||!root) return -1;
    return ctl_owner_load_impl(scripts,parent,entry,name,revision,root,NULL,NULL,out);
}
/* Explicit guarded reads use the SAME parser/lock/public projection. These
 * are snapshots, never a run grant or a replacement for a fresh write audit.
 * Output/revision stay untouched on refusal. Old entry points retain ABI. */
int ctl_owner_load_guarded_at(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_revision **revision,ctl_owner_gate gate,void *ctx,char out[CFG_BYTES+1]) {
    if(parent<0||!gate) return -1;
    return ctl_owner_load_impl(scripts,parent,entry,name,revision,NULL,gate,ctx,out);
}
int ctl_owner_load_guarded_rooted(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_revision **revision,struct ctl_store_root *root,
                   ctl_owner_gate gate,void *ctx,char out[CFG_BYTES+1]) {
    if(parent<0||!root||!gate) return -1;
    return ctl_owner_load_impl(scripts,parent,entry,name,revision,root,gate,ctx,out);
}
static int ctl_owner_apply_composed_impl(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,
                    ctl_scoped_audit scoped_audit,int parent,const char *entry,
                    struct ctl_revision *compare_loaded,const struct ctl_owner_transform *transform,
                    char out[CFG_BYTES+1]) {
    if(!revision||(!request&&(!transform||length||restoring))||!out||(!audit&&!scoped_audit)||!name||!settings_name(name)||
       strcmp(name,revision->name)||(restoring!=0&&restoring!=1)||!ctl_script_dir(scripts)) return -1;
    if(parent>=0&&ctl_directory_current(scripts,parent,entry)) return -2;
    struct ctl_owner_edit *o=calloc(1,sizeof(*o));
    if(!o) return -1;
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
    rc=cfg_store_guarded_locked(o->store,lock,name,ctl_owner_edit_record,ctl_owner_guard,o);
    if(compare_loaded) {
        if(!rc) {
            strcpy(compare_loaded->record,o->committed_record);
            /* A no-op does not materialize a missing record. */
            compare_loaded->record_present=strcmp(o->current,o->committed_record)?1:o->current_present;
        } else if(rc==1) compare_loaded->record_ready=0;
    }
    if(!rc) memcpy(out,o->edit.public_json,strlen(o->edit.public_json)+1);
done:
    if(lock>=0) close(lock);
    if(o->store>=0) close(o->store);
    if(rc<0&&o->refusal) rc=o->refusal;
    free(o); return rc;
}
/* Preserve the old internal call shape and every exported entry. */
static int ctl_owner_apply_impl(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,
                    ctl_scoped_audit scoped_audit,int parent,const char *entry,
                    struct ctl_revision *compare_loaded,char out[CFG_BYTES+1]) {
    return ctl_owner_apply_composed_impl(scripts,name,revision,request,length,restoring,audit,audit_ctx,
        gate,gate_ctx,declarations,declarations_ctx,scoped_audit,parent,entry,compare_loaded,NULL,out);
}
int ctl_owner_apply_anchored(int scripts,int parent,const char *entry,
                    const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_scoped_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,char out[CFG_BYTES+1]) {
    if(parent<0) return -1;
    return ctl_owner_apply_impl(scripts,name,revision,request,length,restoring,NULL,audit_ctx,
                               gate,gate_ctx,declarations,declarations_ctx,audit,parent,entry,NULL,out);
}
/* Strict form edit: compare the entire loaded record under the original lock
 * BEFORE parsing edits or calling any audit. Successful writes advance only
 * this private snapshot from the exact merged bytes, without another read.
 * Uncertain durability invalidates it until an explicit successful load.
 * Content/presence comparison is not a monotonic/ABA-proof revision counter. */
int ctl_owner_apply_loaded_anchored(int scripts,int parent,const char *entry,
                    const char *name,struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_scoped_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,char out[CFG_BYTES+1]) {
    if(parent<0) return -1;
    return ctl_owner_apply_impl(scripts,name,revision,request,length,restoring,NULL,audit_ctx,
                               gate,gate_ctx,declarations,declarations_ctx,audit,parent,entry,revision,out);
}
int ctl_owner_apply_scoped(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_scoped_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,char out[CFG_BYTES+1]) {
    return ctl_owner_apply_impl(scripts,name,revision,request,length,restoring,NULL,audit_ctx,
                               gate,gate_ctx,declarations,declarations_ctx,audit,-1,NULL,NULL,out);
}
int ctl_owner_apply_reviewed(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,
                    ctl_declaration_audit declarations,void *declarations_ctx,char out[CFG_BYTES+1]) {
    return ctl_owner_apply_impl(scripts,name,revision,request,length,restoring,audit,audit_ctx,
                               gate,gate_ctx,declarations,declarations_ctx,NULL,-1,NULL,NULL,out);
}
int ctl_owner_apply_guarded(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,
                    ctl_owner_gate gate,void *gate_ctx,char out[CFG_BYTES+1]) {
    return ctl_owner_apply_reviewed(scripts,name,revision,request,length,restoring,
                                   audit,audit_ctx,gate,gate_ctx,NULL,NULL,out);
}
/* Preserve the previous isolated adapter ABI for existing consumers. */
int ctl_owner_apply(int scripts,const char *name,const struct ctl_revision *revision,
                    const char *request,size_t length,int restoring,
                    ctl_candidate_audit audit,void *audit_ctx,char out[CFG_BYTES+1]) {
    return ctl_owner_apply_guarded(scripts,name,revision,request,length,restoring,
                                  audit,audit_ctx,NULL,NULL,out);
}

/* A31: private connection projection in the ORIGINAL retained record owner.
 * Fixed candidate field mapping belongs here, not in browser JSON or a test
 * admission callback. Every field must be explicitly present in ONE saved
 * record and declared in its captured script. No environment/default mixing.
 * This lends configuration, not a real phone whitelist attestation. */
#include "token-sync-profile.h"
static int ctl_owner_profile_current(int scripts,const struct ctl_revision *r) {
    if(!r||!r->profile_borrowed||!r->record||!r->record_present) return -1;
    int dir=cfg_store_open(0),lock=-1,fd=-1,rc=-1;char actual[CFG_BYTES+1];
    if(dir<0||ctl_store_root_current(r->store_root,dir)||
       (r->scripts_root&&ctl_store_root_current(r->scripts_root,scripts))) goto done;
    lock=openat(dir,".lock",O_WRONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(lock<0||cfg_store_lock_valid(dir,lock)) goto done;
    fd=ctl_revision_open(scripts,r->name,r);
    if(fd>=0&&cfg_store_read(dir,r->name,actual)==1&&!strcmp(actual,r->record)) rc=0;
done:
    memset(actual,0,sizeof(actual));
    if(fd>=0) close(fd);
    if(lock>=0) close(lock);
    if(dir>=0) close(dir);
    return rc;
}
static int ctl_owner_profile_borrow(int scripts,struct ctl_revision *r,struct ts_profile *out) {
    if(!r||!out||!r->profile_ready||r->profile_borrowed) return -1;
    r->profile_ready=0;r->profile_borrowed=1;
    struct ts_profile p={0};xz_json_doc d;int rc=-1;
    static const char *const keys[]={"host","ca_file","token","spki","phone_allowlist","speaker_allowlist","port"};
    if(ctl_owner_profile_current(scripts,r)||xz_json_parse(&d,r->record,strlen(r->record))) goto done;
    /* No bridge field at all is an ordinary recognition-only target. Partial
     * connection records are invalid, never filled from another revision. */
    int found=0;
    for(unsigned i=0;i<7;i++) found+=xz_json_object_get(&d,0,keys[i])>=0;
    if(!found) {rc=1;goto done;}
    if(found!=7) goto done;
    char *dest[]={p.host,p.ca_file,p.token,p.spki,p.phone_allowlist,p.speaker_allowlist};
    size_t caps[]={sizeof(p.host),sizeof(p.ca_file),sizeof(p.token),sizeof(p.spki),sizeof(p.phone_allowlist),sizeof(p.speaker_allowlist)};
    /* ext_read is the original schema parser; do not touch its singleton. */
    struct ext_schema *decl=calloc(1,sizeof(*decl));
    if(!decl) goto done;
    int fd=ctl_revision_open(scripts,r->name,r);
    if(fd<0) {free(decl);goto done;}
    int valid;
#ifdef LX06_NATIVE_TARGET_SETTINGS
    /* The complete-target composition already owns its full-header parser.
     * Never fall back to a 16KiB prefix for this consumer. */
    size_t extent=0;struct cfg *defaults=calloc(1,sizeof(*defaults));
    valid=defaults&&!rts_defaults(r,defaults,&extent)&&rts_extensions(r->bytes,extent,decl)==1;
    free(defaults);
#else
    valid=ext_read(fd,decl)==1;
#endif
    close(fd);
    for(unsigned i=0;valid&&i<7;i++) {
        int f=ext_field_index(decl,keys[i]);char value[2049];
        /* All seven keys were required above. These two opaque lists may be
         * explicitly empty only when the original declared schema accepts it;
         * no absent key/default, wildcard or transport permission is supplied. */
        int list=i==4||i==5;
        valid=f>=0&&ext_text(&d,0,keys[i],value,sizeof(value),!list)&&ext_value(decl,f,value);
        if(valid&&i<6) {
            valid=(list||*value)&&strlen(value)<caps[i];
            if(valid) strcpy(dest[i],value);
        } else if(valid) {
            unsigned n=0;valid=*value!=0;
            for(const char *q=value;valid&&*q;q++) {
                valid=*q>='0'&&*q<='9'&&n<=6553;
                if(valid) {n=n*10+(unsigned)(*q-'0');valid=n<=65535;}
            }
            valid=valid&&n>0;p.port=n;
        }
        if(valid&&i==2) valid=!strcmp(decl->fields[f].type,"password");
        memset(value,0,sizeof(value));
    }
    memset(decl,0,sizeof(*decl));free(decl);
    if(!valid||strlen(p.token)<32||strlen(p.spki)!=64||p.ca_file[0]!='/') goto done;
    for(const char *q=p.token;*q;q++) if(!isalnum((unsigned char)*q)&&*q!='_'&&*q!='-') goto done;
    for(const char *q=p.spki;*q;q++) if(!isxdigit((unsigned char)*q)) goto done;
    if(ctl_owner_profile_current(scripts,r)) goto done;
    memcpy(out,&p,sizeof(p));rc=0;
done:
    memset(&p,0,sizeof(p));
    if(rc) r->profile_borrowed=0;
    return rc;
}

#include "ctl-catalog.inc"

