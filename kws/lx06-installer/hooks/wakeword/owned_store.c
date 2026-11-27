#define _GNU_SOURCE
#include "owned_store.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int invalid(void) { errno=EINVAL; return -1; }
static int check(struct owned_store *s) {
    struct stat d,l,n;
    if (s->opener_pid != getpid()) { errno=ECHILD; return -1; }
    if (fstat(s->dirfd,&d)<0 || fstat(s->lockfd,&l)<0 || fstatat(s->dirfd,"lock",&n,AT_SYMLINK_NOFOLLOW)<0) return -1;
    if (!S_ISDIR(d.st_mode) || d.st_uid!=s->owner || (d.st_mode&07777)!=0700 || d.st_dev!=s->dev || d.st_ino!=s->dirino ||
        !S_ISREG(l.st_mode) || l.st_uid!=s->owner || (l.st_mode&07777)!=0600 || l.st_nlink!=1 ||
        l.st_dev!=s->dev || l.st_ino!=s->lockino || n.st_dev!=l.st_dev || n.st_ino!=l.st_ino || !S_ISREG(n.st_mode)) return invalid();
    return 0;
}
int owned_store_open(struct owned_store *s,int dirfd,dev_t dev,ino_t ino,const unsigned char store_id[OWNED_STORE_ID_SIZE],const unsigned char txn_id[OWNED_STORE_ID_SIZE]) {
    struct stat d,l; int e; unsigned char a=0,b=0; size_t i;
    if (!s || !store_id || !txn_id) return invalid();
    for(i=0;i<OWNED_STORE_ID_SIZE;i++) { a|=store_id[i]; b|=txn_id[i]; }
    if(!a || !b) return invalid();
    s->dirfd=-1; s->lockfd=-1;
    memcpy(s->store_id,store_id,OWNED_STORE_ID_SIZE); memcpy(s->txn_id,txn_id,OWNED_STORE_ID_SIZE);
    s->dirfd=fcntl(dirfd,F_DUPFD_CLOEXEC,3); if(s->dirfd<0) return -1;
    if(fstat(s->dirfd,&d)<0) goto fail;
    s->dev=dev; s->dirino=d.st_ino; s->lockino=ino; s->owner=geteuid(); s->opener_pid=getpid();
    if(d.st_dev!=dev) { invalid(); goto fail; }
    s->lockfd=openat(s->dirfd,"lock",O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); if(s->lockfd<0) goto fail;
    if(fstat(s->lockfd,&l)<0) goto fail;
    if(check(s)<0) goto fail;
    if(flock(s->lockfd,LOCK_EX|LOCK_NB)<0) goto fail;
    if(check(s)<0) goto fail;
    return 0;
fail: e=errno; owned_store_close(s); errno=e; return -1;
}
void owned_store_close(struct owned_store *s) {
    if(!s) return;
    if(s->lockfd>=0) close(s->lockfd);
    if(s->dirfd>=0) close(s->dirfd);
    s->dirfd=-1; s->lockfd=-1;
}
/* Wire v1: magic[8], store/transaction IDs, pinned dev/dir/lock u64 LE,
 * operation byte, three fixed entries, then CRC32 LE. No native struct bytes. */
static void put64(unsigned char *p,uint64_t v) { int i; for(i=0;i<8;i++) { p[i]=(unsigned char)v; v>>=8; } }
static uint64_t get64(const unsigned char *p) { int i; uint64_t v=0; for(i=7;i>=0;i--) v=(v<<8)|p[i]; return v; }
static uint32_t manifest_crc(const unsigned char *p,size_t n) {
    uint32_t c=0xffffffffU; size_t i; int b;
    for(i=0;i<n;i++) { c^=p[i]; for(b=0;b<8;b++) c=(c>>1)^(0xedb88320U & (0U-(c&1U))); }
    return ~c;
}
static int manifest_name(const char *p) {
    size_t n=strnlen(p,OWNED_MANIFEST_NAME_SIZE),i;
    if(!n || n==OWNED_MANIFEST_NAME_SIZE || p[0]=='.' || !strcmp(p,"lock")) return 0;
    for(i=0;i<n;i++) if((unsigned char)p[i]<32 || p[i]=='/' || p[i]==127) return 0;
    return 1;
}
int owned_store_manifest_encode(struct owned_store *s,const struct owned_store_manifest *m,unsigned char *out,size_t len) {
    unsigned char b[OWNED_MANIFEST_SIZE]; const char *names[6]; uint32_t crc; size_t i,j,pos=65;
    if(!s || !m || !out || len!=sizeof(b) || m->operation!=OWNED_MANIFEST_RENAME) return invalid();
    if(check(s)<0) return -1;
    for(i=0;i<3;i++) {
        const struct owned_manifest_entry *e=&m->entry[i];
        names[2*i]=e->old_name; names[2*i+1]=e->new_name;
        if(!manifest_name(e->old_name) || !manifest_name(e->new_name) || !e->old_ino || !e->new_ino || e->old_dev!=(uint64_t)s->dev || e->new_dev!=(uint64_t)s->dev) return invalid();
    }
    for(i=0;i<6;i++) for(j=i+1;j<6;j++) if(!strcmp(names[i],names[j])) return invalid();
    memset(b,0,sizeof(b)); memcpy(b,"LXOMAN01",8); memcpy(b+8,s->store_id,16); memcpy(b+24,s->txn_id,16);
    put64(b+40,(uint64_t)s->dev); put64(b+48,(uint64_t)s->dirino); put64(b+56,(uint64_t)s->lockino); b[64]=(unsigned char)m->operation;
    for(i=0;i<3;i++) {
        const struct owned_manifest_entry *e=&m->entry[i];
        memcpy(b+pos,e->old_name,strlen(e->old_name)); pos+=64;
        memcpy(b+pos,e->new_name,strlen(e->new_name)); pos+=64;
        put64(b+pos,e->old_dev); pos+=8; put64(b+pos,e->old_ino); pos+=8;
        put64(b+pos,e->new_dev); pos+=8; put64(b+pos,e->new_ino); pos+=8;
    }
    crc=manifest_crc(b,pos); for(i=0;i<4;i++) b[pos+i]=(unsigned char)(crc>>(8*i));
    if(check(s)<0) return -1;
    memcpy(out,b,sizeof(b)); return 0;
}
int owned_store_manifest_validate(struct owned_store *s,const unsigned char *in,size_t len,struct owned_store_manifest *out) {
    struct owned_store_manifest m; unsigned char canonical[OWNED_MANIFEST_SIZE]; size_t i,pos=65;
    if(!s || !in || !out || len!=OWNED_MANIFEST_SIZE) return invalid();
    memset(&m,0,sizeof(m)); m.operation=in[64];
    for(i=0;i<3;i++) {
        struct owned_manifest_entry *e=&m.entry[i];
        memcpy(e->old_name,in+pos,64); pos+=64; memcpy(e->new_name,in+pos,64); pos+=64;
        e->old_dev=get64(in+pos); pos+=8; e->old_ino=get64(in+pos); pos+=8;
        e->new_dev=get64(in+pos); pos+=8; e->new_ino=get64(in+pos); pos+=8;
    }
    if(owned_store_manifest_encode(s,&m,canonical,sizeof(canonical))<0) return -1;
    if(memcmp(in,canonical,sizeof(canonical))) return invalid();
    *out=m; return 0;
}
void owned_store_unpin(struct owned_store_pins *p) {
    int i; if(!p) return; for(i=0;i<6;i++) { if(p->fd[i]>=0) close(p->fd[i]); p->fd[i]=-1; }
}
int owned_store_pin_manifest(struct owned_store *s,const unsigned char *b,size_t len,struct owned_store_pins *p) {
    struct owned_store_manifest m; struct stat st,n; int i,k,j,e;
    if(!p) return invalid();
    for(i=0;i<6;i++) p->fd[i]=-1;
    if(owned_store_manifest_validate(s,b,len,&m)<0) return -1;
    for(i=0;i<3;i++) for(k=0;k<2;k++) {
        const struct owned_manifest_entry *x=&m.entry[i]; const char *name=k?x->new_name:x->old_name;
        uint64_t dev=k?x->new_dev:x->old_dev, ino=k?x->new_ino:x->old_ino;
        j=2*i+k; p->fd[j]=openat(s->dirfd,name,O_PATH|O_NOFOLLOW|O_CLOEXEC);
        if(p->fd[j]<0 || fstat(p->fd[j],&st)<0 || fstatat(s->dirfd,name,&n,AT_SYMLINK_NOFOLLOW)<0) goto fail;
        if((uint64_t)st.st_dev!=dev || (uint64_t)st.st_ino!=ino || n.st_dev!=st.st_dev || n.st_ino!=st.st_ino ||
           (i==0 ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink!=1))) { invalid(); goto fail; }
    }
    if(check(s)<0) goto fail;
    return 0;
fail: e=errno; owned_store_unpin(p); errno=e; return -1;
}
/* Atomic create-only publication: interrupted pre-link work has no name;
 * interrupted post-link publication is either absent or a complete snapshot.
 * Caller must supply the independently retained expected bytes. */
int owned_store_bind(struct owned_store *s,const unsigned char *b,size_t len) {
    struct owned_store_pins p; int fd=-1,e,phase; size_t pos=0; ssize_t n;
    if(owned_store_phase(s,&phase)<0) return -1;
    if(phase) return invalid();
    if(owned_store_pin_manifest(s,b,len,&p)<0) return -1;
    fd=openat(s->dirfd,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600); if(fd<0) goto fail;
    while(pos<len) { n=write(fd,b+pos,len-pos); if(n<0 && errno==EINTR) continue; if(n<=0) { if(!n) errno=EIO; goto fail; } pos+=(size_t)n; }
    if(fsync(fd)<0 || check(s)<0) goto fail;
    if(linkat(fd,"",s->dirfd,".owned-manifest",AT_EMPTY_PATH)<0) goto fail;
    if(fsync(s->dirfd)<0 || check(s)<0) goto fail;
    if(close(fd)<0) { fd=-1; goto fail; } owned_store_unpin(&p); return 0;
fail: e=errno; if(fd>=0) close(fd); owned_store_unpin(&p); errno=e; return -1;
}
int owned_store_bound_phase(struct owned_store *s,const unsigned char *b,size_t len,int *phase) {
    struct owned_store_pins p; struct stat st; unsigned char got[OWNED_MANIFEST_SIZE+1];
    size_t pos=0; ssize_t n; int fd=-1,e,current;
    if(!phase || !b || len!=OWNED_MANIFEST_SIZE) return invalid();
    if(owned_store_pin_manifest(s,b,len,&p)<0) return -1;
    fd=openat(s->dirfd,".owned-manifest",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); if(fd<0) goto fail;
    if(fstat(fd,&st)<0) goto fail;
    if(!S_ISREG(st.st_mode) || st.st_dev!=s->dev || st.st_uid!=s->owner || st.st_nlink!=1 || (st.st_mode&07777)!=0600) { invalid(); goto fail; }
    while(pos<sizeof(got)) { n=read(fd,got+pos,sizeof(got)-pos); if(n<0 && errno==EINTR) continue; if(n<0) goto fail; if(!n) break; pos+=(size_t)n; }
    if(pos!=len || memcmp(got,b,len)) { invalid(); goto fail; }
    if(owned_store_phase(s,&current)<0) goto fail;
    /* Existing V2 phase records are NOT manifest-bound. Refuse replay. */
    if(current!=0) { invalid(); goto fail; }
    if(check(s)<0) goto fail;
    if(close(fd)<0) { fd=-1; goto fail; } owned_store_unpin(&p); *phase=current; return 0;
fail: e=errno; if(fd>=0) close(fd); owned_store_unpin(&p); errno=e; return -1;
}
static size_t format_record(struct owned_store *s,int phase,char *out,size_t cap) {
    char a[33], b[33]; static const char hex[]="0123456789abcdef"; size_t i; int n;
    for(i=0;i<OWNED_STORE_ID_SIZE;i++) { a[2*i]=hex[s->store_id[i]>>4]; a[2*i+1]=hex[s->store_id[i]&15]; b[2*i]=hex[s->txn_id[i]>>4]; b[2*i+1]=hex[s->txn_id[i]&15]; }
    a[32]=b[32]=0;
    n=snprintf(out,cap,"LX06-OWNED-PHASE-V2-%d-%s-%s-%" PRIxMAX "-%" PRIxMAX "-%" PRIxMAX "\n",phase,a,b,(uintmax_t)s->dev,(uintmax_t)s->dirino,(uintmax_t)s->lockino);
    return n<0 || (size_t)n>=cap ? 0 : (size_t)n;
}
static int read_record(struct owned_store *s,int phase) {
    char name[32],expected[192],buf[193]; struct stat st; int fd, e; ssize_t n; size_t pos=0, len;
    snprintf(name,sizeof(name),".owned-phase-%d",phase);
    len=format_record(s,phase,expected,sizeof(expected)); if(!len) return invalid();
    fd=openat(s->dirfd,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); if(fd<0) return -1;
    if(fstat(fd,&st)<0) goto fail;
    if(!S_ISREG(st.st_mode) || st.st_dev!=s->dev || st.st_uid!=s->owner || st.st_nlink!=1 || (st.st_mode&07777)!=0600) { invalid(); goto fail; }
    while(pos<sizeof(buf)) { n=read(fd,buf+pos,sizeof(buf)-pos); if(n<0 && errno==EINTR) continue; if(n<0) goto fail; if(!n) break; pos+=(size_t)n; }
    if(pos!=len || memcmp(buf,expected,len)!=0) { invalid(); goto fail; }
    if(fsync(fd)<0) goto fail;
    if(close(fd)<0) return -1;
    return 0;
fail: e=errno; close(fd); errno=e; return -1;
}
int owned_store_phase(struct owned_store *s,int *phase) {
    int i, found=0, absent=0, dupfd, e; DIR *dir; struct dirent *ent;
    if(!s || !phase) return invalid();
    if(check(s)<0) return -1;
    /* Reject all unknown names in the reserved journal namespace. */
    dupfd=openat(s->dirfd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); if(dupfd<0) return -1;
    dir=fdopendir(dupfd); if(!dir) { e=errno; close(dupfd); errno=e; return -1; }
    errno=0;
    while((ent=readdir(dir))!=NULL) {
        if(!strncmp(ent->d_name,".owned-phase-",13) && strcmp(ent->d_name,".owned-phase-1") && strcmp(ent->d_name,".owned-phase-2") && strcmp(ent->d_name,".owned-phase-3")) { closedir(dir); return invalid(); }
        errno=0;
    }
    e=errno; if(closedir(dir)<0 && !e) e=errno; if(e) { errno=e; return -1; }
    for(i=1;i<=3;i++) {
        if(read_record(s,i)==0) { if(absent) return invalid(); found=i; }
        else if(errno==ENOENT) absent=1; else return -1;
    }
    if(fsync(s->dirfd)<0 || check(s)<0) return -1;
    *phase=found; return 0;
}
int owned_store_append(struct owned_store *s,int phase,owned_store_checkpoint cb,void *arg) {
    char name[32],record[192]; size_t pos=0,len; int current,fd,e; ssize_t n;
    if(!s || phase<1 || phase>3) return invalid();
    if(owned_store_phase(s,&current)<0) return -1;
    if(phase!=current+1) return invalid();
    snprintf(name,sizeof(name),".owned-phase-%d",phase);
    len=format_record(s,phase,record,sizeof(record)); if(!len) return invalid();
    fd=openat(s->dirfd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); if(fd<0) return -1;
    if(cb) cb("created",arg);
    while(pos<len) { n=write(fd,record+pos,len-pos); if(n<0 && errno==EINTR) continue; if(n<=0) { if(!n) errno=EIO; goto fail; } pos+=(size_t)n; }
    if(cb) cb("written",arg);
    if(fsync(fd)<0) goto fail;
    if(cb) cb("file-synced",arg);
    if(close(fd)<0) return -1;
    if(fsync(s->dirfd)<0) return -1;
    if(cb) cb("dir-synced",arg);
    return check(s);
fail: e=errno; close(fd); errno=e; return -1;
}
