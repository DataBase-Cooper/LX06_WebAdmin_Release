/* A-owned host fixture: inspect the flat uploaded-script directory and the
 * ORIGINAL settings store while borrowing its existing exclusive lock.
 * No new lock, record, parser, server, script execution or production entry. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T24 catalog is fixture-only"
#endif
#include <dirent.h>
#define CTL_CATALOG_COUNT 32u
#define CTL_CATALOG_BYTES (2u*1024u*1024u)
struct ctl_catalog_entry {
    struct ctl_revision *revision;
    char record[CFG_BYTES+1];
    int record_present;
};
struct ctl_catalog {
    int scripts,store,lock,parent;
    struct ctl_store_root *store_root;
    char entry[256];
    char names[CTL_CATALOG_COUNT][65],target[65];
    size_t count,used;
    struct ctl_catalog_entry entries[CTL_CATALOG_COUNT];
};
static int ctl_catalog_names(int scripts,char names[CTL_CATALOG_COUNT][65],size_t *count) {
    if(!ctl_script_dir(scripts)) return -1;
    /* dup() shares a directory offset; open a fresh description on every scan. */
    int fd=openat(scripts,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR *dir=fd<0?NULL:fdopendir(fd); struct dirent *de; int rc=-1;
    if(!dir) { if(fd>=0) close(fd); return -1; }
    *count=0;
    for(;;) {
        errno=0; de=readdir(dir);
        if(!de) { if(!errno) rc=0; break; }
        if(!settings_name(de->d_name)) continue; /* same managed-name boundary */
        if(*count==CTL_CATALOG_COUNT) break; /* refuse, never truncate */
        strcpy(names[(*count)++],de->d_name);
    }
    closedir(dir);
    if(rc) return -1;
    /* Stable byte order for private enumeration, independent of readdir order. */
    for(size_t i=1;i<*count;i++) {
        char name[65]; strcpy(name,names[i]); size_t j=i;
        while(j&&strcmp(names[j-1],name)>0) { strcpy(names[j],names[j-1]); j--; }
        strcpy(names[j],name);
    }
    return 0;
}
void ctl_catalog_release(struct ctl_catalog *c) {
    if(!c) return;
    for(size_t i=0;i<c->count;i++) ctl_owner_release(c->entries[i].revision);
    /* Duplicated lock is the SAME open file description: release only after
     * the entire native commit/uncertainty boundary, never LOCK_UN here. */
    if(c->lock>=0) close(c->lock);
    if(c->store>=0) close(c->store);
    if(c->scripts>=0) close(c->scripts);
    if(c->parent>=0) close(c->parent);
    ctl_store_root_release(c->store_root);
    volatile unsigned char *p=(volatile unsigned char *)c;
    for(size_t i=0;i<sizeof(*c);i++) p[i]=0;
    free(c);
}
int ctl_catalog_current(const struct ctl_catalog *c) {
    char names[CTL_CATALOG_COUNT][65],record[CFG_BYTES+1]; size_t count;
    if(!c||ctl_store_root_current(c->store_root,c->store)||
       (c->parent>=0&&ctl_directory_current(c->scripts,c->parent,c->entry))||
       cfg_store_lock_valid(c->store,c->lock)||
       ctl_catalog_names(c->scripts,names,&count)||count!=c->count) return -1;
    for(size_t i=0;i<count;i++) {
        if(strcmp(names[i],c->names[i])) return -1;
        if(!strcmp(names[i],c->target)) continue; /* native target guard owns it */
        int fd=ctl_revision_open(c->scripts,names[i],c->entries[i].revision);
        if(fd<0) return -1;
        close(fd);
        int present=cfg_store_read(c->store,names[i],record);
        if(present<0||present!=c->entries[i].record_present||strcmp(record,c->entries[i].record)) return -1;
    }
    return 0;
}
static struct ctl_catalog *ctl_catalog_acquire_impl(void *borrowed_scope,size_t retained_bytes) {
    struct ctl_owner_edit *o=borrowed_scope;
    if(!o||!o->scope_active||retained_bytes>CTL_CATALOG_BYTES||cfg_store_lock_valid(o->store,o->lock)) return NULL;
    struct ctl_catalog *c=calloc(1,sizeof(*c));
    if(!c) return NULL;
    c->scripts=c->store=c->lock=c->parent=-1;
    c->used=retained_bytes;
    c->store_root=ctl_store_root_retain(o->store_root);
    if(ctl_owner_directory(o)) goto bad;
    if(o->scripts_parent>=0) {
        c->parent=fcntl(o->scripts_parent,F_DUPFD_CLOEXEC,0);
        if(c->parent<0) goto bad;
        strcpy(c->entry,o->scripts_entry); /* validated <=255 above */
    }
    c->scripts=fcntl(o->scripts,F_DUPFD_CLOEXEC,0);
    c->store=fcntl(o->store,F_DUPFD_CLOEXEC,0);
    c->lock=fcntl(o->lock,F_DUPFD_CLOEXEC,0);
    strcpy(c->target,o->name);
    if(c->scripts<0||c->store<0||c->lock<0||
       ctl_catalog_names(c->scripts,c->names,&c->count)) goto bad;
    int target_found=0;
    for(size_t i=0;i<c->count;i++) {
        const char *name=c->names[i];
        if(!strcmp(name,c->target)) { target_found=1; continue; }
        /* Validate size against the WHOLE catalog budget BEFORE allocation. */
        SETTINGS_STAT st; int fd=ctl_script_open(c->scripts,name,&st);
        if(fd<0) goto bad;
        close(fd);
        if((uintmax_t)st.st_size>CTL_CATALOG_BYTES-c->used) goto bad;
        struct ctl_revision *r=ctl_capture(c->scripts,name);
        c->entries[i].revision=r;
        if(!r||r->size>CTL_CATALOG_BYTES-c->used) goto bad;
        c->used+=r->size;
        c->entries[i].record_present=cfg_store_read(c->store,name,c->entries[i].record);
        if(c->entries[i].record_present<0) goto bad;
    }
    if(!target_found||ctl_catalog_current(c)) goto bad;
    return c;
bad:
    ctl_catalog_release(c); return NULL;
}
struct ctl_catalog *ctl_catalog_acquire(void *borrowed_scope) {
    return ctl_catalog_acquire_impl(borrowed_scope,0);
}
size_t ctl_catalog_count(const struct ctl_catalog *c) { return c?c->count:0; }
const char *ctl_catalog_name(const struct ctl_catalog *c,size_t index) {
    return c&&index<c->count&&strcmp(c->names[index],c->target)?c->names[index]:NULL;
}
static int ctl_catalog_entry_public(const struct ctl_catalog *c,size_t index,char out[CFG_BYTES+1]) {
    if(!out||!ctl_catalog_name(c,index)) return -1;
    struct ctl_map *map=calloc(1,sizeof(*map));
    struct cfg *defaults=malloc(sizeof(*defaults)),*effective=malloc(sizeof(*effective));
    char *public=malloc(CFG_BYTES+1); int rc=-1;
    const struct ctl_revision *r=c->entries[index].revision;
    if(!map||!defaults||!effective||!public) goto done;
    size_t n=r->size>CFG_BYTES?CFG_BYTES:r->size;
    if(cfg_parse(r->bytes,n,defaults)||ctl_stored_map(c->entries[index].record,map)||
       ctl_effective(defaults,map,effective)||ctl_public(effective,map,public)) goto done;
    memcpy(out,public,strlen(public)+1); rc=0;
done:
    free(map); free(defaults); free(effective); free(public); return rc;
}
int ctl_catalog_public(const struct ctl_catalog *c,size_t index,char out[CFG_BYTES+1]) {
    if(ctl_catalog_current(c)) return -1;
    return ctl_catalog_entry_public(c,index,out);
}
