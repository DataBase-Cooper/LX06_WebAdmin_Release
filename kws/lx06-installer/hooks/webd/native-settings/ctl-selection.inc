/* A-owned selected catalog, HOST FIXTURE ONLY. Include after ctl-owner.inc.
 * Reuses original lock/capture/settings/cfg, and generated webd appmeta/name
 * functions. No runtime parser, new record, service or production registration. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T24 selection is fixture-only"
#endif
struct ctl_selection_root { int dir,parent; const char *entry; };
struct ctl_selection {
    struct ctl_catalog *catalog;
    struct ctl_selection_root roots[3];
    struct ctl_store_root *ancestors[3];
    char entries[3][256];
    char names[2][CTL_CATALOG_COUNT][65]; size_t counts[2];
    unsigned source[CTL_CATALOG_COUNT];
    char meta[16384]; size_t meta_size; int meta_exists;
    SETTINGS_STAT meta_stat;
};
static int ctl_selection_root_current(const struct ctl_selection_root *r) {
    if(r->dir>=0) return ctl_directory_current(r->dir,r->parent,r->entry);
    SETTINGS_STAT st; struct stat named;
    if(r->parent<0||!r->entry||!*r->entry||strchr(r->entry,'/')||
       !strcmp(r->entry,".")||!strcmp(r->entry,"..")||strlen(r->entry)>255||
       SETTINGS_FSTAT(r->parent,&st)||!S_ISDIR(st.st_mode)||
       !((st.st_uid==getuid()&&!(st.st_mode&0022))||
         ((st.st_uid==0||st.st_uid==getuid())&&(st.st_mode&S_ISVTX)))) return -1;
    errno=0;
    return fstatat(r->parent,r->entry,&named,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT?0:-1;
}
static int ctl_selection_meta(int dir,char bytes[16384],size_t *size,
                              int *exists,SETTINGS_STAT *st) {
    int fd=openat(dir,".appmeta",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) { if(errno!=ENOENT) return -1; *exists=0; *size=0; bytes[0]=0; return 0; }
    SETTINGS_STAT after; int rc=-1;
    if(SETTINGS_FSTAT(fd,st)||!S_ISREG(st->st_mode)||st->st_uid!=getuid()||
       st->st_nlink!=1||(st->st_mode&0022)||st->st_size<0||st->st_size>=16383) goto done;
    *size=(size_t)st->st_size; *exists=1;
    if(ctl_pread_exact(fd,bytes,*size,0)||SETTINGS_FSTAT(fd,&after)||!ctl_same_stat(st,&after)) goto done;
    bytes[*size]=0; rc=0;
done: close(fd); return rc;
}
static long ctl_selection_meta_read(const struct ctl_selection *selection,char *out,size_t size) {
    if(!selection->meta_exists||selection->meta_size>=size-1) return -1;
    memcpy(out,selection->meta,selection->meta_size+1); return (long)selection->meta_size;
}
/* Exact pinned webd bodies with only context argument/read-provider adapted. */
#include "selection-appmeta.generated.h"
int ctl_selection_target(const struct ctl_selection_root roots[3],const char *name) {
    if(!roots||!name||!app_name_safe(name)) return -1;
    for(unsigned i=0;i<3;i++) if(ctl_selection_root_current(&roots[i])) return -1;
    for(unsigned i=0;i<2;i++) {
        if(roots[i].dir<0) continue;
        struct stat st;
        if(fstatat(roots[i].dir,name,&st,AT_SYMLINK_NOFOLLOW)) {
            if(errno==ENOENT) continue;
            return -1;
        }
        SETTINGS_STAT safe; int fd=ctl_script_open(roots[i].dir,name,&safe);
        /* Unlike display-only webd fallback, unsafe existing candidates refuse
         * this editable fixture instead of silently selecting another file. */
        if(fd<0) return -1;
        close(fd); return (int)i;
    }
    return -1;
}
void ctl_selection_release(struct ctl_selection *s) {
    if(!s) return;
    for(unsigned i=0;i<3;i++) {
        if(s->roots[i].dir>=0) close(s->roots[i].dir);
        if(s->roots[i].parent>=0) close(s->roots[i].parent);
        ctl_store_root_release(s->ancestors[i]);
    }
    ctl_catalog_release(s->catalog);
    volatile unsigned char *p=(volatile unsigned char *)s;
    for(size_t i=0;i<sizeof(*s);i++) p[i]=0;
    free(s);
}
int ctl_selection_current(const struct ctl_selection *s) {
    if(!s||ctl_store_root_current(s->catalog->store_root,s->catalog->store)||
       cfg_store_lock_valid(s->catalog->store,s->catalog->lock)) return -1;
    for(unsigned i=0;i<3;i++) {
        int fd=s->roots[i].dir>=0?s->roots[i].dir:s->roots[i].parent;
        if((s->ancestors[i]&&ctl_store_root_current(s->ancestors[i],fd))||
           ctl_selection_root_current(&s->roots[i])) return -1;
    }
    char meta[16384]; size_t size; int exists; SETTINGS_STAT st;
    if(ctl_selection_meta(s->roots[2].dir,meta,&size,&exists,&st)||
       exists!=s->meta_exists||size!=s->meta_size||memcmp(meta,s->meta,size)||
       (exists&&!ctl_same_stat(&st,&s->meta_stat))) return -1;
    for(unsigned i=0;i<2;i++) {
        char names[CTL_CATALOG_COUNT][65]; size_t count=0;
        if(s->roots[i].dir>=0&&ctl_catalog_names(s->roots[i].dir,names,&count)) return -1;
        if(count!=s->counts[i]) return -1;
        for(size_t j=0;j<count;j++) if(strcmp(names[j],s->names[i][j])) return -1;
    }
    for(size_t i=0;i<s->catalog->count;i++) {
        const char *name=s->catalog->names[i];
        if(ctl_selection_target(s->roots,name)!=(int)s->source[i]) return -1;
        if(!strcmp(name,s->catalog->target)) continue;
        int fd=ctl_revision_open(s->roots[s->source[i]].dir,name,s->catalog->entries[i].revision);
        char record[CFG_BYTES+1];
        if(fd<0) return -1;
        close(fd);
        int present=cfg_store_read(s->catalog->store,name,record);
        if(present<0||present!=s->catalog->entries[i].record_present||
           strcmp(record,s->catalog->entries[i].record)) return -1;
    }
    return 0;
}
static int ctl_selection_add(struct ctl_selection *s,const char *name) {
    struct ctl_catalog *c=s->catalog;
    if(!app_name_safe(name)) return 0;
    for(size_t i=0;i<c->count;i++) if(!strcmp(c->names[i],name)) return 0;
    /* Metadata-only ghosts are display/management entries, not executable
     * candidates. Both directory name sets remain dependencies. */
    int present=0;
    for(unsigned r=0;r<2;r++) for(size_t i=0;i<s->counts[r];i++)
        if(!strcmp(s->names[r][i],name)) present=1;
    if(!present) return 0;
    int source=ctl_selection_target(s->roots,name);
    if(source<0||c->count==CTL_CATALOG_COUNT) return -1;
    size_t index=c->count++; strcpy(c->names[index],name); s->source[index]=(unsigned)source;
    return 0;
}
static struct ctl_selection *ctl_selection_capture(void *scope,const struct ctl_selection_root roots[3],
                      struct ctl_store_root *const ancestors[3],size_t retained_bytes,int editing) {
    struct ctl_owner_edit *o=scope;
    if(!o||!roots||!o->scope_active||retained_bytes>CTL_CATALOG_BYTES||
       cfg_store_lock_valid(o->store,o->lock)) return NULL;
    struct ctl_selection *s=calloc(1,sizeof(*s));
    if(!s) return NULL;
    for(unsigned i=0;i<3;i++) s->roots[i].dir=s->roots[i].parent=-1;
    s->catalog=calloc(1,sizeof(*s->catalog));
    if(!s->catalog) goto bad;
    struct ctl_catalog *c=s->catalog;
    c->scripts=c->store=c->lock=c->parent=-1;
    c->used=retained_bytes;
    c->store_root=ctl_store_root_retain(o->store_root);
    for(unsigned i=0;i<3;i++) {
        if(ctl_selection_root_current(&roots[i])) goto bad;
        if(ancestors) {
            int fd=roots[i].dir>=0?roots[i].dir:roots[i].parent;
            if(!ancestors[i]||ctl_store_root_current(ancestors[i],fd)) goto bad;
            s->ancestors[i]=ctl_store_root_retain(ancestors[i]);
        }
        strcpy(s->entries[i],roots[i].entry); s->roots[i].entry=s->entries[i];
        s->roots[i].parent=fcntl(roots[i].parent,F_DUPFD_CLOEXEC,0);
        if(roots[i].dir>=0) s->roots[i].dir=fcntl(roots[i].dir,F_DUPFD_CLOEXEC,0);
        if(s->roots[i].parent<0||(roots[i].dir>=0&&s->roots[i].dir<0)) goto bad;
    }
    if(s->roots[2].dir<0) goto bad;
    if(editing) {
        int target=ctl_selection_target(s->roots,o->name); SETTINGS_STAT selected,actual;
        if(target<0||SETTINGS_FSTAT(s->roots[target].dir,&selected)||SETTINGS_FSTAT(o->scripts,&actual)||
           selected.st_dev!=actual.st_dev||selected.st_ino!=actual.st_ino) goto bad;
    }
    c->store=fcntl(o->store,F_DUPFD_CLOEXEC,0); c->lock=fcntl(o->lock,F_DUPFD_CLOEXEC,0);
    if(c->store<0||c->lock<0) goto bad;
    /* A writer snapshot has no separately loaded editor target: capture ALL
     * selected scripts/records, including the one being uninstalled if present.
     * Empty catalog is valid after the final executable has been removed. */
    if(editing) strcpy(c->target,o->name);
    if(ctl_selection_meta(s->roots[2].dir,s->meta,&s->meta_size,&s->meta_exists,&s->meta_stat)) goto bad;
    for(unsigned i=0;i<2;i++) if(s->roots[i].dir>=0&&
        ctl_catalog_names(s->roots[i].dir,s->names[i],&s->counts[i])) goto bad;
    int count=appmeta_count(s);
    for(int i=0;i<count;i++) {
        char name[65]; long epoch;
        if(!appmeta_get(s,i,name,sizeof(name),&epoch)&&ctl_selection_add(s,name)) goto bad;
    }
    for(unsigned r=0;r<2;r++) for(size_t i=0;i<s->counts[r];i++)
        if(ctl_selection_add(s,s->names[r][i])) goto bad;
    int target_found=0;
    for(size_t i=0;i<c->count;i++) {
        const char *name=c->names[i];
        if(!strcmp(name,c->target)) { target_found=1; continue; }
        SETTINGS_STAT st; int fd=ctl_script_open(s->roots[s->source[i]].dir,name,&st);
        if(fd<0) goto bad;
        close(fd);
        if((uintmax_t)st.st_size>CTL_CATALOG_BYTES-c->used) goto bad;
        struct ctl_revision *revision=ctl_capture(s->roots[s->source[i]].dir,name);
        c->entries[i].revision=revision;
        if(!revision||revision->size>CTL_CATALOG_BYTES-c->used) goto bad;
        c->used+=revision->size;
        c->entries[i].record_present=cfg_store_read(c->store,name,c->entries[i].record);
        if(c->entries[i].record_present<0) goto bad;
    }
    if((editing&&!target_found)||ctl_selection_current(s)) goto bad;
    return s;
bad: ctl_selection_release(s); return NULL;
}
static struct ctl_selection *ctl_selection_acquire_budget(void *scope,const struct ctl_selection_root roots[3],
                                                      struct ctl_store_root *const ancestors[3],size_t retained_bytes) {
    return ctl_selection_capture(scope,roots,ancestors,retained_bytes,1);
}
static struct ctl_selection *ctl_selection_acquire_impl(void *scope,const struct ctl_selection_root roots[3],
                                                      struct ctl_store_root *const ancestors[3]) {
    return ctl_selection_acquire_budget(scope,roots,ancestors,0);
}
/* Retain the old immediate-parent ABI for existing fixture consumers. */
struct ctl_selection *ctl_selection_acquire(void *scope,const struct ctl_selection_root roots[3]) {
    return ctl_selection_acquire_impl(scope,roots,NULL);
}
struct ctl_selection *ctl_selection_acquire_rooted(void *scope,const struct ctl_selection_root roots[3],
                                                 struct ctl_store_root *const ancestors[3]) {
    if(!ancestors) return NULL;
    return ctl_selection_acquire_impl(scope,roots,ancestors);
}
size_t ctl_selection_count(const struct ctl_selection *s) { return s?s->catalog->count:0; }
const char *ctl_selection_name(const struct ctl_selection *s,size_t i) {
    return s?ctl_catalog_name(s->catalog,i):NULL;
}
int ctl_selection_public(const struct ctl_selection *s,size_t i,char out[CFG_BYTES+1]) {
    if(!s||ctl_selection_current(s)) return -1;
    return ctl_catalog_entry_public(s->catalog,i,out);
}
