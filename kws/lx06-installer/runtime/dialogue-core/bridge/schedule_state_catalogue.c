#define _GNU_SOURCE
#include "schedule_state_catalogue.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
typedef struct stat64 catalogue_stat;
extern int __fxstat64(int,int,struct stat64 *);
extern int __fxstatat64(int,int,const char *,struct stat64 *,int);
#define catalogue_fstat(fd,st) __fxstat64(3,(fd),(st))
#define catalogue_fstatat(fd,path,st,flags) __fxstatat64(3,(fd),(path),(st),(flags))
#else
typedef struct stat catalogue_stat;
#define catalogue_fstat fstat
#define catalogue_fstatat fstatat
#endif
static int same(const catalogue_stat *a,const catalogue_stat *b){
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&
        a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_nlink==b->st_nlink&&
        a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&
        a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int directory(const catalogue_stat *st){
    return S_ISDIR(st->st_mode)&&st->st_uid==geteuid()&&(st->st_mode&07777)==0700;
}
static int private_file(const catalogue_stat *st){
    return S_ISREG(st->st_mode)&&st->st_uid==geteuid()&&
        (st->st_mode&07777)==0600&&st->st_nlink==1;
}
static int component(const char *name){
    if(!name||!*name||!strcmp(name,".")||!strcmp(name,".."))return 0;
    size_t n=0;while(n<256&&name[n]){if(name[n]=='/')return 0;n++;}
    return n&&n<256;
}
static int overlaps(const void *a,size_t na,const void *b,size_t nb){
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if(!a||!b||!na||!nb)return 0;
    return x<=y?y-x<na:x-y<nb;
}
static int add(size_t *value,size_t amount){
    if(amount>SIZE_MAX-*value)return 0;
    *value+=amount;return 1;
}
typedef int (*entry_visitor)(void *,int,const char *);
/* Independent open, never dup/lseek caller's directory OFD. Linux getdents64
 * record fields are read with memcpy, without unaligned integer casts. */
static int entries(int borrowed,entry_visitor visit,void *ctx){
    int fd=openat(borrowed,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    catalogue_stat opened,base;int ok=!catalogue_fstat(fd,&opened)&&
        !catalogue_fstat(borrowed,&base)&&same(&opened,&base);
    unsigned char buffer[512];
    while(ok){
        ssize_t n;do{n=syscall(SYS_getdents64,fd,buffer,sizeof buffer);}while(n<0&&errno==EINTR);
        if(n<0||n>(ssize_t)sizeof buffer){ok=0;break;}if(!n)break;
        size_t at=0;
        while(ok&&at<(size_t)n){
            /* ino64/off64/reclen16/type8/name; fixed prefix19 bytes. */
            if((size_t)n-at<20){ok=0;break;}
            uint16_t length;memcpy(&length,buffer+at+16,sizeof length);
            if(length<20||length>(size_t)n-at){ok=0;break;}
            const char *name=(const char *)buffer+at+19;
            if(!memchr(name,0,length-19)){ok=0;break;}
            if(strcmp(name,".")&&strcmp(name,"..")){
                if(!component(name)||visit(ctx,borrowed,name)!=1)ok=0;
            }
            at+=length;
        }
    }
    memset(buffer,0,sizeof buffer);if(close(fd))ok=0;return ok;
}
typedef struct { unsigned bits;catalogue_stat state,lock; } child_files;
static int child_entry(void *p,int fd,const char *name){
    child_files *files=p;catalogue_stat st;
    unsigned bit=!strcmp(name,"schedule.v1")?1u:!strcmp(name,"schedule.lock")?2u:0u;
    if(!bit||(files->bits&bit)||catalogue_fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)||
       !private_file(&st))return -1;
    files->bits|=bit;if(bit==1)files->state=st;else files->lock=st;return 1;
}
static int files_equal(const child_files *a,const child_files *b){
    return a->bits==b->bits&&(!(a->bits&1u)||same(&a->state,&b->state))&&
        (!(a->bits&2u)||same(&a->lock,&b->lock));
}
/* Verify real borrowed lease identity and a current exclusive conflict.
 * No EX acquisition or lock operation is performed on the borrowed FD. */
static int lease(int root,int held,catalogue_stat *observed){
    catalogue_stat a,b,c;int fd=-1,ok=0;
    if(catalogue_fstat(held,&a)||!private_file(&a)||
       catalogue_fstatat(root,"schedule-global.lock",&b,AT_SYMLINK_NOFOLLOW)||!same(&a,&b))return 0;
    fd=openat(root,"schedule-global.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0||catalogue_fstat(fd,&c)||!same(&a,&c))goto done;
    errno=0;int rc=flock(fd,LOCK_SH|LOCK_NB),error=errno;
    if(rc==0||!(error==EWOULDBLOCK||error==EAGAIN))goto done;
    if(catalogue_fstat(held,&c)||!same(&a,&c)||
       catalogue_fstatat(root,"schedule-global.lock",&b,AT_SYMLINK_NOFOLLOW)||!same(&a,&b))goto done;
    if(observed)*observed=a;
    ok=1;
done:if(fd>=0&&close(fd))ok=0;return ok;
}
typedef struct {
    int held_global_lock;unsigned lease_seen;
    size_t limit;br_schedule_workspace *workspace;
    br_schedule_catalogue_resolver resolve;void *resolve_context;
    br_schedule_catalogue_visitor visit;void *visit_context;
    br_schedule_catalogue_totals totals;
} scan;
static int child(void *p,int root,const char *name){
    scan *s=p;catalogue_stat named,opened,after;int fd=-1,ok=0;
    if(!strcmp(name,"schedule-global.lock")){
        if(s->lease_seen||!lease(root,s->held_global_lock,NULL))return -1;
        s->lease_seen=1;return 1;
    }
    child_files before={0},end={0};br_store *owned=NULL;
    br_store_observation store={0};br_schedule_observation state={0};
    if(s->totals.directories>=s->limit||
       catalogue_fstatat(root,name,&named,AT_SYMLINK_NOFOLLOW)||!directory(&named))return -1;
    fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0||catalogue_fstat(fd,&opened)||!same(&opened,&named)||
       !entries(fd,child_entry,&before))goto done;
    int resolved=s->resolve(s->resolve_context,fd,&owned),present=-1;
    if(resolved==1){
        catalogue_stat actual;
        if(!owned||owned->schedule!=BR_STORE_SCHEDULE||owned->poisoned||
           catalogue_fstat(owned->directory,&actual)||!same(&actual,&opened))goto done;
        present=br_store_observe_owned(owned,s->workspace->bytes,BR_SCHEDULE_BYTES,&store);
    }else if(resolved==0&&!owned){
        present=br_store_observe_at(fd,BR_STORE_SCHEDULE,
            s->workspace->bytes,BR_SCHEDULE_BYTES,&store);
    }else goto done;
    if(present!=0&&present!=1)goto done;
    if(present==1&&(store.kind!=BR_STORE_SCHEDULE||
       store.payload_bytes>BR_SCHEDULE_BYTES||store.framed_bytes<store.payload_bytes||
       br_schedule_probe_bytes(s->workspace->bytes,store.payload_bytes,NULL,0,&state)!=1||
       state.payload_bytes!=store.payload_bytes))goto done;
    if(s->visit&&s->visit(s->visit_context,fd,present,&store,&state)!=1)goto done;
    /* A callback may observe only. Recheck real directory and named entries
     * after it; no quota is published from a swapped/mutated child. */
    if(!entries(fd,child_entry,&end)||!files_equal(&before,&end)||
       catalogue_fstat(fd,&after)||!same(&opened,&after)||
       catalogue_fstatat(root,name,&named,AT_SYMLINK_NOFOLLOW)||!same(&after,&named))goto done;
    if(!add(&s->totals.directories,1)||!add(present?&s->totals.present:&s->totals.absent,1)||
       !add(&s->totals.tasks,state.task_count)||!add(&s->totals.history,state.history_count)||
       !add(&s->totals.payload_bytes,store.payload_bytes)||
       !add(&s->totals.framed_bytes,store.framed_bytes)||
       !add(&s->totals.active,state.active_count)||!add(&s->totals.terminal,state.terminal_count))goto done;
    ok=1;
done:
    memset(s->workspace->bytes,0,sizeof s->workspace->bytes);
    if(fd>=0&&close(fd))ok=0;
    return ok?1:-1;
}
int br_schedule_state_catalogue_observe(int root,int parent,const char *entry,int held_global_lock_fd,
    size_t max_entries,br_schedule_workspace *workspace,
    br_schedule_catalogue_resolver resolve,void *resolve_context,
    br_schedule_catalogue_visitor visit,void *visit_context,
    br_schedule_catalogue_totals *out){
    size_t entry_bytes=entry?strnlen(entry,256)+1:0;
    /* Invalid aliases cannot be zeroed without damaging borrowed input. */
    if(!out||overlaps(out,sizeof(*out),workspace,workspace?sizeof(*workspace):0)||
       overlaps(out,sizeof(*out),entry,entry_bytes))return -1;
    memset(out,0,sizeof(*out));
    if(!workspace||overlaps(workspace,sizeof(*workspace),entry,entry_bytes)||
       workspace->busy||!resolve||!component(entry)||
       !max_entries||max_entries>BR_SCHEDULE_CATALOGUE_ENTRIES)return -1;
    char anchor[256];memcpy(anchor,entry,strlen(entry)+1);
    catalogue_stat initial,parent_before,named,after,parent_after,lease_before,lease_after;
    if(catalogue_fstat(root,&initial)||!directory(&initial)||
       catalogue_fstat(parent,&parent_before)||!directory(&parent_before)||
       catalogue_fstatat(parent,anchor,&named,AT_SYMLINK_NOFOLLOW)||!same(&initial,&named)||
       !lease(root,held_global_lock_fd,&lease_before))return -1;
    workspace->busy=1;
    scan s={.held_global_lock=held_global_lock_fd,.limit=max_entries,.workspace=workspace,.resolve=resolve,
        .resolve_context=resolve_context,.visit=visit,.visit_context=visit_context};
    int ok=entries(root,child,&s)&&s.lease_seen==1&&!catalogue_fstat(root,&after)&&same(&initial,&after)&&
        !catalogue_fstat(parent,&parent_after)&&same(&parent_before,&parent_after)&&
        !catalogue_fstatat(parent,anchor,&named,AT_SYMLINK_NOFOLLOW)&&same(&after,&named)&&
        lease(root,held_global_lock_fd,&lease_after)&&same(&lease_before,&lease_after);
    memset(workspace->bytes,0,sizeof workspace->bytes);workspace->busy=0;
    if(ok)*out=s.totals;
    memset(&s,0,sizeof s);memset(anchor,0,sizeof anchor);
    return ok?1:-1;
}
