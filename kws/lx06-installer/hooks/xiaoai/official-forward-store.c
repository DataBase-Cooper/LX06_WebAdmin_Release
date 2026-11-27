#define _GNU_SOURCE
#include "official-forward-store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

/* Stock 32-bit ARM glibc exports __fxstat64, not modern fstat.
 * Its version 3 layout is the time32 struct stat64 ABI. */
#if defined(__arm__) && defined(__GLIBC__) && __SIZEOF_POINTER__ == 4
#if defined(__USE_TIME_BITS64)
#error "official-forward-store requires the stock ARM time32 stat64 ABI"
#endif
extern int __fxstat64(int, int, struct stat64 *);
#define OF_STAT struct stat64
#define OF_FSTAT(fd, st) __fxstat64(3, (fd), (st))
#else
#define OF_STAT struct stat
#define OF_FSTAT(fd, st) fstat((fd), (st))
#endif

#define STORE_FILE "official-forward"
static int directory_ok(int d) {
    OF_STAT s;
    return !OF_FSTAT(d,&s) && S_ISDIR(s.st_mode) && s.st_uid==geteuid() &&
           (s.st_mode & 07777)==0700;
}
static int name_ok(const char *s) {
    if (!s || !*s || *s=='.') return 0;
    size_t n=strnlen(s,65);
    if (!n || n>64) return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
              (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')) return 0;
    }
    return 1;
}
int of_store_read(int d,char out[65]) {
    char b[67]; size_t n=0; ssize_t z; OF_STAT s;
    if (!out || !directory_ok(d)) { errno=EPERM; return -1; }
    int f=openat(d,STORE_FILE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(f<0) { if(errno==ENOENT) { strcpy(out,"official"); return 0; } return -1; }
    if(OF_FSTAT(f,&s) || !S_ISREG(s.st_mode) || s.st_uid!=geteuid() ||
       /* A concurrently replaced file can have link count zero after open. */
       s.st_nlink>1 || (s.st_mode&07777)!=0600 || s.st_size<2 || s.st_size>65) {
        close(f); errno=EPERM; return -1;
    }
    while(n<sizeof b) {
        z=read(f,b+n,sizeof b-n);
        if(z<0 && errno==EINTR) continue;
        if(z<0) { int e=errno; close(f); errno=e; return -1; }
        if(!z) break;
        n+=(size_t)z;
    }
    if(close(f) || n!=(size_t)s.st_size || n<2 || n>65 || b[n-1]!='\n' || memchr(b,0,n)) { errno=EINVAL; return -1; }
    b[n-1]=0;
    if(!name_ok(b)) { errno=EINVAL; return -1; }
    memcpy(out,b,n); return 0;
}
static int select_locked(int d,const char *selection,of_store_lookup lookup,void *context) {
    char old[65],tmp[64],line[66]; int fd=-1, renamed=0, e;
    if(!name_ok(selection)) { errno=EINVAL; return -1; }
    if(strcmp(selection,"official") && (!lookup || lookup(context,selection)!=1)) { errno=EINVAL; return -1; }
    if(of_store_read(d,old)) return -1;
    for(unsigned i=0;i<128;i++) {
        snprintf(tmp,sizeof tmp,".official-forward.%ld.%u",(long)getpid(),i);
        fd=openat(d,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0 || errno!=EEXIST) break;
    }
    if(fd<0) return -1;
    size_t n=(size_t)snprintf(line,sizeof line,"%s\n",selection), off=0;
    if(fchmod(fd,0600)) goto fail;
    while(off<n) {
        ssize_t z=write(fd,line+off,n-off);
        if(z<0 && errno==EINTR) continue;
        if(z<=0) { if(!z) errno=EIO; goto fail; }
        off+=(size_t)z;
    }
    if(fsync(fd)) goto fail;
    if(close(fd)) { fd=-1; goto fail; }
    fd=-1;
    if(renameat(d,tmp,d,STORE_FILE)) goto fail;
    renamed=1;
    if(fsync(d)) goto fail;
    return 0;
fail:
    e=errno; if(fd>=0) close(fd); if(!renamed) unlinkat(d,tmp,0); errno=e; return -1;
}
/* Unlock the shared open description even if a callback forked a child. */
static int unlock_directory(int l) {
    int r, e;
    do { r=flock(l,LOCK_UN); } while(r<0 && errno==EINTR);
    e=errno; close(l); errno=e; return r;
}
/* A fresh open file description is essential: dup(d) would share flock state. */
static int lock_directory(int d) {
    if (!directory_ok(d)) { errno=EPERM; return -1; }
    int l=openat(d,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (l<0) return -1;
    if (!directory_ok(l) || flock(l,LOCK_EX)) {
        int e=errno; close(l); errno=e ? e : EPERM; return -1;
    }
    return l;
}
int of_store_select(int d,const char *selection,of_store_lookup lookup,void *context) {
    int l=lock_directory(d), r, e;
    if(l<0) return -1;
    r=select_locked(l,selection,lookup,context); e=errno;
    if(unlock_directory(l) && !r) return -1;
    errno=e;
    return r;
}
int of_store_snapshot(int d,struct of_store_snapshot *out) {
    int l, f, e;
    if(!out) { errno=EINVAL; return -1; }
    out->directory_fd=-1; out->file_fd=-1; out->selection[0]=0;
    l=lock_directory(d); if(l<0) return -1;
    if(of_store_read(l,out->selection)) goto fail_snapshot;
    f=openat(l,STORE_FILE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(f<0 && errno!=ENOENT) goto fail_snapshot;
    out->file_fd=f;
    /* Retain the directory inode, but not the locked open description. */
    out->directory_fd=openat(l,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(out->directory_fd<0) goto fail_snapshot;
    if(unlock_directory(l)) {
        e=errno; close(out->directory_fd); close(out->file_fd);
        out->directory_fd=-1; out->file_fd=-1; out->selection[0]=0; errno=e; return -1;
    }
    return 0;
fail_snapshot:
    e=errno; if(out->file_fd>=0) close(out->file_fd);
    out->file_fd=-1; out->selection[0]=0; unlock_directory(l); errno=e; return -1;
}
void of_store_snapshot_close(struct of_store_snapshot *s) {
    if(!s) return;
    if(s->file_fd>=0) close(s->file_fd);
    if(s->directory_fd>=0) close(s->directory_fd);
    s->file_fd=-1; s->directory_fd=-1; s->selection[0]=0;
}
int of_store_reserve(int d,const struct of_store_snapshot *s,of_store_commit commit,void *context) {
    OF_STAT a,b; char current[65]; int l, f=-1, e, r=-1;
    if(!s || !commit || s->directory_fd<0 || !name_ok(s->selection)) { errno=EINVAL; return -1; }
    l=lock_directory(d); if(l<0) return -1;
    if(OF_FSTAT(l,&a) || OF_FSTAT(s->directory_fd,&b)) goto done;
    if(a.st_dev!=b.st_dev || a.st_ino!=b.st_ino) { errno=ESTALE; goto done; }
    if(of_store_read(l,current)) goto done;
    f=openat(l,STORE_FILE,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(f<0 && errno!=ENOENT) goto done;
    if((f<0)!=(s->file_fd<0)) { errno=ESTALE; goto done; }
    if(f>=0) {
        if(OF_FSTAT(f,&a) || OF_FSTAT(s->file_fd,&b)) goto done;
        if(a.st_dev!=b.st_dev || a.st_ino!=b.st_ino) { errno=ESTALE; goto done; }
    }
    if(strcmp(current,s->selection)) { errno=ESTALE; goto done; }
    if(commit(context,current)!=1) { errno=ECANCELED; goto done; }
    r=0;
done:
    e=errno; if(f>=0) close(f);
    if(unlock_directory(l) && !r) return -1;
    errno=e; return r;
}
enum of_result of_store_resolve(int d,of_store_lookup lookup,void *context,char out[65]) {
    if(of_store_read(d,out)) return OF_REJECT;
    if(!strcmp(out,"official")) return of_decide(1,1,out,0,0);
    int valid=lookup ? lookup(context,out) : 0;
    return of_decide(1,1,out,valid,valid);
}
