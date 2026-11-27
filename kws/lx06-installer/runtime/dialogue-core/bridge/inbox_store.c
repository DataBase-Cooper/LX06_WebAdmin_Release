#define _GNU_SOURCE
#include "inbox_store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* Same stock ARM stat ABI discipline as the existing TTS implementation.
 * Modern headers otherwise import fstat/fstatat@GLIBC_2.33, absent on OEM2.25.
 * This symbol selection still requires K7 final link + operator ARM loading.
 */
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
typedef struct stat64 store_stat;
extern int __fxstat64(int,int,struct stat64 *);
extern int __fxstatat64(int,int,const char *,struct stat64 *,int);
#define store_fstat(fd,st) __fxstat64(3,(fd),(st))
#define store_fstatat(fd,path,st,flags) __fxstatat64(3,(fd),(path),(st),(flags))
#else
typedef struct stat store_stat;
#define store_fstat fstat
#define store_fstatat fstatat
#endif

static const char *const names[]={"result-inbox.v1","schedule.v1","flow-journal.v1","flow-owner.v1"};
static const char *const temps[]={"result-inbox.v1.tmp","schedule.v1.tmp","flow-journal.v1.tmp","flow-owner.v1.tmp"};
static const char *const locks[]={"result-inbox.lock","schedule.lock","flow-journal.lock","flow-owner.lock"};
static const char *const magics[]={"LX06BOX1","LX06SCH1","LX06FLW1","LX06OWS1"};
static size_t limit(const br_store *s){return s->schedule==3?7480u:s->schedule==2?40960u:s->schedule==1?16384u:BR_STORE_LIMIT;}
#define name names[s->schedule]
#define temp temps[s->schedule]
#define lockname locks[s->schedule]
static int directory_ok(int fd){
    store_stat st;
    return !store_fstat(fd,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==geteuid()&&(st.st_mode&07777)==0700;
}
static int private_file(const store_stat *st){
    return S_ISREG(st->st_mode)&&st->st_uid==geteuid()&&(st->st_mode&07777)==0600&&st->st_nlink==1;
}
/* 1 private regular file, 0 absent, -1 unsafe/error. Never follows symlinks. */
static int named(int directory,const char *path){
    store_stat st;
    if(store_fstatat(directory,path,&st,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-1;
    return private_file(&st)?1:-1;
}
static int checked(int directory,const char *path,int fd){
    store_stat a,b;
    return !store_fstat(fd,&a)&&private_file(&a)&&!store_fstatat(directory,path,&b,AT_SYMLINK_NOFOLLOW)&&
        a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&private_file(&b);
}
static int open_store(br_store *s,int directory,int schedule){
    if(!s)return -1;
    *s=(br_store){.directory=-1,.lock=-1,.schedule=schedule};
    if(!directory_ok(directory))return -1;
    s->directory=fcntl(directory,F_DUPFD_CLOEXEC,3);
    if(s->directory<0)return -1;
    s->lock=openat(s->directory,lockname,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(s->lock<0||!checked(s->directory,lockname,s->lock)||flock(s->lock,LOCK_EX|LOCK_NB))goto failed;
    /* A crashed writer may have left an incomplete temp. Under the lock, only
     * remove an exact private file. Committed state is never reset here. */
    int stale=named(s->directory,temp);
    if(stale<0||(stale&&(unlinkat(s->directory,temp,0)||fsync(s->directory))))goto failed;
    return 0;
failed:br_store_close(s);return -1;
}
int br_store_open(br_store *s,int directory){return open_store(s,directory,0);}
int br_store_open_schedule(br_store *s,int directory){return open_store(s,directory,1);}
int br_store_open_flow(br_store *s,int directory){return open_store(s,directory,2);}
int br_store_open_owner(br_store *s,int directory){return open_store(s,directory,3);}
void br_store_close(br_store *s){
    if(!s)return;
    if(s->guard&&s->guard->closing)s->guard->closing(s->guard->context,s);
    if(s->lock>=0)close(s->lock);
    if(s->directory>=0)close(s->directory);
    *s=(br_store){.directory=-1,.lock=-1};
}
static int ready(br_store *s){
    return s&&s->schedule>=0&&s->schedule<=3&&s->directory>=0&&s->lock>=0&&!s->poisoned&&directory_ok(s->directory)&&checked(s->directory,lockname,s->lock);
}
static uint32_t crc32_update(uint32_t x,const unsigned char *p,size_t n){
    while(n--){x^=*p++;for(unsigned i=0;i<8;i++)x=(x>>1)^(0xedb88320u& (0u-(x&1u)));}
    return x;
}
static uint32_t crc32(const unsigned char *p,size_t n){return ~crc32_update(UINT32_MAX,p,n);}
static void put32(unsigned char *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static uint32_t get32(const unsigned char *p){uint32_t n=0;for(unsigned i=0;i<4;i++)n|=(uint32_t)p[i]<<(8*i);return n;}
static int transfer(int fd,void *data,size_t n,int writing){
    unsigned char *p=data;
    while(n){
        ssize_t done=writing?write(fd,p,n):read(fd,p,n);
        if(done<0&&errno==EINTR)continue;
        if(done<=0)return -1;
        p+=done;n-=(size_t)done;
    }
    return 0;
}
static int read_original(br_store *s,void *data,size_t capacity,size_t *length){
    if(length)*length=0;
    if(!length||(!data&&capacity))return -1;
    int present=named(s->directory,name);
    if(present<=0)return present;
    int fd=openat(s->directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return -1;
    unsigned char header[24];store_stat st;int rc=-1;
    if(!checked(s->directory,name,fd)||store_fstat(fd,&st)||transfer(fd,header,sizeof header,0))goto end;
    uint32_t n=get32(header+12);
    if(memcmp(header,magics[s->schedule],8)||get32(header+8)!=1||get32(header+20)!=0||
       n>limit(s)||(data&&n>capacity)||st.st_size!=(off_t)(sizeof header+n))goto end;
    uint32_t checksum=UINT32_MAX;
    if(data){
        if(transfer(fd,data,n,0))goto end;
        checksum=crc32_update(checksum,data,n);
    }else{
        unsigned char chunk[512];size_t left=n;
        while(left){
            size_t take=left<sizeof(chunk)?left:sizeof(chunk);
            if(transfer(fd,chunk,take,0)){memset(chunk,0,sizeof(chunk));goto end;}
            checksum=crc32_update(checksum,chunk,take);left-=take;
        }
        memset(chunk,0,sizeof(chunk));
    }
    if(~checksum!=get32(header+16)||!checked(s->directory,name,fd))goto end;
    *length=n;rc=1;
end:if(close(fd)){*length=0;return -1;}return rc;
}
int br_store_read(br_store *s,void *data,size_t capacity,size_t *length){
    if(length)*length=0;
    if(!ready(s)||!data)return -1;
    return read_original(s,data,capacity,length);
}
static int observation_output(void *data,size_t capacity,br_store_observation *out){
    if(!out)return 0;
    uintptr_t a=(uintptr_t)data,b=(uintptr_t)out;
    if(data&&(a<=b?b-a<capacity:a-b<sizeof(*out)))return 0;
    memset(out,0,sizeof(*out));return 1;
}
static int observe_locked(br_store *s,void *data,size_t capacity,br_store_observation *out){
    /* A temp denotes an unresolved write. Observation never cleans it up. */
    if(named(s->directory,temp)!=0)return -1;
    size_t bytes=0;int rc=read_original(s,data,capacity,&bytes);
    if(rc==1){
        if(named(s->directory,temp)!=0||!directory_ok(s->directory))return -1;
        *out=(br_store_observation){.payload_bytes=bytes,.framed_bytes=bytes+24u,
            .kind=(unsigned)s->schedule};
    }
    return rc;
}
int br_store_observe_owned(br_store *s,void *data,size_t capacity,br_store_observation *out){
    if(s&&out){
        uintptr_t a=(uintptr_t)s,b=(uintptr_t)out;
        if(a<=b?b-a<sizeof(*s):a-b<sizeof(*out))return -1;
    }
    if(s&&data){
        uintptr_t a=(uintptr_t)s,b=(uintptr_t)data;
        if(a<=b?b-a<sizeof(*s):a-b<capacity)return -1;
    }
    if(!observation_output(data,capacity,out)||!ready(s))return -1;
    int rc=observe_locked(s,data,capacity,out);
    if(!ready(s))rc=-1;
    if(rc!=1)memset(out,0,sizeof(*out));
    return rc;
}
int br_store_observe_at(int directory,unsigned kind,void *data,size_t capacity,br_store_observation *out){
    if(!observation_output(data,capacity,out)||kind>BR_STORE_OWNER||!directory_ok(directory))return -1;
    br_store state={.directory=directory,.lock=-1,.schedule=(int)kind},*s=&state;
    if(named(directory,temp)!=0)return -1;
    int locked=named(directory,lockname);
    if(locked<0)return -1;
    if(!locked)return named(directory,name)==0?0:-1;
    s->lock=openat(directory,lockname,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(s->lock<0)return -1;
    int rc=-1;
    if(checked(directory,lockname,s->lock)&&!flock(s->lock,LOCK_SH|LOCK_NB)){
        rc=observe_locked(s,data,capacity,out);
        if(!checked(directory,lockname,s->lock)||!directory_ok(directory))rc=-1;
    }
    if(close(s->lock))rc=-1;
    if(rc!=1)memset(out,0,sizeof(*out));
    return rc;
}
static int write_original(br_store *s,const void *data,size_t length){
    if(!ready(s)||!data||length>limit(s)||named(s->directory,name)<0)return -1;
    int fd=openat(s->directory,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    unsigned char header[24]={0};memcpy(header,magics[s->schedule],8);
    put32(header+8,1);put32(header+12,(uint32_t)length);put32(header+16,crc32(data,length));
    int bad=!checked(s->directory,temp,fd)||transfer(fd,header,sizeof header,1)||
        transfer(fd,(void *)data,length,1)||fsync(fd);
    if(close(fd))bad=1;
    if(!bad&&(named(s->directory,name)<0||renameat(s->directory,temp,s->directory,name)))bad=1;
    if(bad){unlinkat(s->directory,temp,0);return -1;}
    if(fsync(s->directory)){s->poisoned=1;return -1;}
    return 0;
}

int br_store_guard_bind(br_store *s,const br_store_guard *guard){
    if(!ready(s)||s->guard||!guard||!guard->before_write||!guard->after_write||!guard->closing)return -1;
    uintptr_t a=(uintptr_t)s,b=(uintptr_t)guard;
    if(a<=b?b-a<sizeof(*s):a-b<sizeof(*guard))return -1;
    s->guard=guard;return 1;
}
int br_store_guard_unbind(br_store *s,const br_store_guard *guard){
    if(!s||s->guard!=guard||!guard||s->directory<0||s->lock<0||
       !directory_ok(s->directory)||!checked(s->directory,lockname,s->lock))return -1;
    s->guard=NULL;return 1;
}
int br_store_write(br_store *s,const void *data,size_t length){
    if(!ready(s)||!data||length>limit(s))return -1;
    const br_store_guard *guard=s->guard;
    if(guard&&guard->before_write(guard->context,s,data,length)!=1)return -1;
    int rc=write_original(s,data,length);
    if(guard&&guard->after_write(guard->context,s,rc)!=1){s->poisoned=1;return -1;}
    return rc;
}
