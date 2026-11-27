/* Shared complete Linux directory scan. No parser, registry or idle authority. */
#ifndef LX06_SCHEDULE_DIRECTORY_SCAN_H
#define LX06_SCHEDULE_DIRECTORY_SCAN_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "schedule-hash.h"
#define DIRECTORY_NAMES_BYTES 65536u
#define DIRECTORY_CONFIG_BYTES 65536u
struct directory_access { void *context;int directory[3];int (*anchor)(void *,unsigned); };
/* Constant memory lexical enumeration. getdents64 avoids fdopendir's hidden
 * libc directory-buffer allocation. Rescanning trades time for bounded space;
 * no name is omitted, and the same encoded name budget is still enforced. */
struct directory_names { char name[256];size_t used,count; };
struct directory_entry { uint64_t inode;int64_t offset;unsigned short length;unsigned char type;char name[]; };
_Static_assert(offsetof(struct directory_entry,name)==19,"Linux getdents64 ABI");
static int directory_script(const char *s){
    if(!s||!*s||*s=='.'||strlen(s)>64||strstr(s,".."))return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
        (*s>='0'&&*s<='9')||strchr("._-",*s)))return 0;
    return 1;
}
static int directory_same(const struct stat *a,const struct stat *b,int times){
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&
        a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&a->st_nlink==b->st_nlink&&
        (!times||(a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&
        a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&
        a->st_ctim.tv_nsec==b->st_ctim.tv_nsec));
}
static void directory_names_free(struct directory_names *n){
    memset(n,0,sizeof(*n));
}
static int directory_next(int fd,int scripts,struct directory_names *n){
    char next[256]={0};unsigned char bytes[512];int ok=1;
    /* An independent open description never advances the owner's directory. */
    int copy=openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(copy<0)return -1;
    for(;;){
        long got=syscall(SYS_getdents64,copy,bytes,sizeof bytes);
        if(got<0&&errno==EINTR)continue;
        if(got<0||(unsigned long)got>sizeof bytes){ok=0;break;}
        if(!got)break;
        size_t at=0;
        while(at<(size_t)got){
            size_t header=offsetof(struct directory_entry,name);unsigned short length=0;
            if((size_t)got-at<header+1){ok=0;break;}
            memcpy(&length,bytes+at+offsetof(struct directory_entry,length),sizeof length);
            if(length<header+1||length>(size_t)got-at){ok=0;break;}
            const char *name=(const char *)bytes+at+header;
            const char *end=memchr(name,0,length-header);
            if(!end||end==name||(size_t)(end-name)>=sizeof next){ok=0;break;}
            if(strcmp(name,".")&&strcmp(name,"..")&&(!scripts||directory_script(name))&&
               strcmp(name,n->name)>0&&(!*next||strcmp(name,next)<0))strcpy(next,name);
            at+=length;
        }
        if(!ok)break;
    }
    if(close(copy))ok=0;memset(bytes,0,sizeof bytes);if(!ok)return -1;
    if(!*next)return 0;
    size_t size=strlen(next)+1;
    if(n->used+size+2*(n->count+1)>DIRECTORY_NAMES_BYTES)return -1;
    strcpy(n->name,next);n->used+=size;n->count++;return 1;
}
static int directory_carrier(const char *name,char script[65]){
    size_t n=strlen(name);if(n<=5||n>69||strcmp(name+n-5,".json"))return 0;
    memcpy(script,name,n-5);script[n-5]=0;return directory_script(script);
}
static int directory_regular(const struct stat *s,int private){
    return S_ISREG(s->st_mode)&&s->st_uid==(private?geteuid():0)&&s->st_nlink==1&&
        !(s->st_mode&(private?0077:0022));
}
static int directory_scan_snapshot(const struct directory_access *d,unsigned i,unsigned char out[32],
    struct directory_names *keep,size_t *encoded){
    struct stat before_directory,after_directory;
    if(!d->anchor(d->context,i)||fstat(d->directory[i],&before_directory))return 0;
    struct directory_names n={0};int step=0;
    struct eo_schedule_hash hash;int ok=eo_schedule_hash_init(&hash)==1;size_t bytes=0;
    while(ok&&(step=directory_next(d->directory[i],i==0,&n))==1){
        const char *name=n.name;struct stat s;
        if(fstatat(d->directory[i],name,&s,AT_SYMLINK_NOFOLLOW)){ok=0;break;}
        uint64_t fields[]={s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_gid,s.st_nlink,
            (uint64_t)s.st_size,(uint64_t)s.st_mtim.tv_sec,(uint64_t)s.st_mtim.tv_nsec,
            (uint64_t)s.st_ctim.tv_sec,(uint64_t)s.st_ctim.tv_nsec};
        ok=eo_schedule_hash_update(&hash,name,strlen(name)+1)==1&&eo_schedule_hash_update(&hash,fields,sizeof fields)==1;
        if(i==2){
            char script[65];struct stat deployed;
            if(!directory_carrier(name,script)||!directory_regular(&s,1)||s.st_size<=0||
               (uint64_t)s.st_size>DIRECTORY_CONFIG_BYTES-bytes||
               fstatat(d->directory[0],script,&deployed,AT_SYMLINK_NOFOLLOW)||!directory_regular(&deployed,0)){
                ok=0;break;
            }
            bytes+=(size_t)s.st_size;
            /* Empty carriers are not retained as parser views. Hash every
             * carrier's real bytes here as well as its immutable identity.
             * Stream in small chunks; this scan also runs with a live FLOW. */
            int file=openat(d->directory[i],name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(file<0){ok=0;break;}struct stat before,after;char chunk[512];off_t at=0;
            if(fstat(file,&before)||!directory_same(&before,&s,1))ok=0;
            while(ok&&at<s.st_size){size_t take=(size_t)(s.st_size-at);if(take>sizeof chunk)take=sizeof chunk;
                ssize_t got=pread(file,chunk,take,at);if(got<=0){ok=0;break;}
                ok=eo_schedule_hash_update(&hash,chunk,(size_t)got)==1;at+=got;}
            memset(chunk,0,sizeof chunk);
            if(fstat(file,&after)||!directory_same(&after,&s,1))ok=0;close(file);
        }
    }
    if(step<0)ok=0;
    if(ok)ok=!fstat(d->directory[i],&after_directory)&&directory_same(&before_directory,&after_directory,1)&&
        d->anchor(d->context,i)&&eo_schedule_hash_done(&hash,out)==1;
    memset(&hash,0,sizeof hash);
    if(ok&&encoded)*encoded=bytes;
    if(ok&&keep)*keep=n;else directory_names_free(&n);return ok;
}
#endif
