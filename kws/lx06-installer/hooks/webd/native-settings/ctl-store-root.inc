/* T24 HOST FIXTURE ONLY. Pin the configured settings path from / without
 * following ANY symlink. Reuse its original .lock/store; never create roots.
 * Synchronous references let a catalog outlive the form revision safely. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T24 store root is fixture-only"
#endif
#define CTL_STORE_DEPTH 32u
struct ctl_store_root {
    unsigned refs;
    size_t count;
    int fds[CTL_STORE_DEPTH+1];
    SETTINGS_STAT stats[CTL_STORE_DEPTH+1];
    char entries[CTL_STORE_DEPTH][256];
};
static void ctl_store_root_release(struct ctl_store_root *r) {
    if(!r||--r->refs) return;
    for(size_t i=0;i<=r->count;i++) if(r->fds[i]>=0) close(r->fds[i]);
    free(r);
}
static struct ctl_store_root *ctl_store_root_retain(struct ctl_store_root *r) {
    if(r) r->refs++;
    return r;
}
static int ctl_store_identity(const SETTINGS_STAT *a,const SETTINGS_STAT *b) {
    /* Contents/timestamps/nlink can change during legitimate settings writes.
     * Directory replacement, chmod and ownership changes invalidate the root. */
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&
           a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid;
}
static int ctl_store_ancestor(const SETTINGS_STAT *st) {
    return S_ISDIR(st->st_mode)&&(st->st_uid==0||st->st_uid==getuid())&&
           (!(st->st_mode&0022)||(st->st_mode&S_ISVTX));
}
static int ctl_store_root_current(const struct ctl_store_root *r,int store) {
    SETTINGS_STAT st;
    if(!r||SETTINGS_FSTAT(store,&st)||
       !ctl_store_identity(&r->stats[r->count],&st)) return -1;
    for(size_t i=0;i<=r->count;i++) {
        if(SETTINGS_FSTAT(r->fds[i],&st)||!ctl_store_identity(&r->stats[i],&st)) return -1;
        if(i) {
            struct stat named;
            if(fstatat(r->fds[i-1],r->entries[i-1],&named,AT_SYMLINK_NOFOLLOW)||
               named.st_dev!=st.st_dev||named.st_ino!=st.st_ino||
               named.st_mode!=st.st_mode||named.st_uid!=st.st_uid||named.st_gid!=st.st_gid) return -1;
        }
    }
    return 0;
}
static struct ctl_store_root *ctl_path_root_open_impl(const char *path,int store,int private) {
    if(!path||path[0]!='/'||(private&&!path[1])||strlen(path)>4095) return NULL;
    struct ctl_store_root *r=calloc(1,sizeof(*r));
    if(!r) return NULL;
    r->refs=1;
    for(size_t i=0;i<=CTL_STORE_DEPTH;i++) r->fds[i]=-1;
    int flags=O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC;
    r->fds[0]=open("/",flags);
    if(r->fds[0]<0||SETTINGS_FSTAT(r->fds[0],&r->stats[0])||
       !ctl_store_ancestor(&r->stats[0])) goto bad;
    path++;
    while(*path) {
        const char *slash=strchr(path,'/'); size_t n=slash?(size_t)(slash-path):strlen(path);
        if(!n||n>255||r->count==CTL_STORE_DEPTH||
           (n==1&&path[0]=='.')||(n==2&&path[0]=='.'&&path[1]=='.')) goto bad;
        memcpy(r->entries[r->count],path,n);
        int fd=openat(r->fds[r->count],r->entries[r->count],flags);
        if(fd<0) goto bad;
        r->fds[++r->count]=fd;
        if(SETTINGS_FSTAT(fd,&r->stats[r->count])||!ctl_store_ancestor(&r->stats[r->count])) goto bad;
        if(!slash) break;
        path=slash+1;
        if(!*path) goto bad;
    }
    const SETTINGS_STAT *last=&r->stats[r->count];
    if((private&&(last->st_uid!=getuid()||(last->st_mode&0777)!=0700))||
       ctl_store_root_current(r,store)) goto bad;
    return r;
bad:
    ctl_store_root_release(r); return NULL;
}
static struct ctl_store_root *ctl_store_root_open(int store) {
    return ctl_path_root_open_impl(LX06_SETTINGS_DIR,store,1);
}
/* Private host handles only. An absent script root pins its existing parent;
 * selection separately checks the missing immediate entry. Never mkdir. */
struct ctl_store_root *ctl_path_root_open(const char *path,int directory) {
    return ctl_path_root_open_impl(path,directory,0);
}
void ctl_path_root_release(struct ctl_store_root *root) { ctl_store_root_release(root); }
