/* Small private DATA store shared by the settings helper and official router.
 * All paths are opened relative to a checked directory; never follow links or
 * shell-source configuration. Missing is distinct from corrupt/untrusted. */
#ifndef LX06_SCRIPT_SETTINGS_STORE_H
#define LX06_SCRIPT_SETTINGS_STORE_H
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#if defined(SETTINGS_LEGACY_ABI) && defined(__arm__)
/* Stock dynamic glibc 2.25 exports __fxstat64, not modern fstat. The router
 * is statically linked and deliberately keeps its normal libc ABI. */
#define SETTINGS_STAT struct stat64
extern int __fxstat64(int,int,struct stat64*);
extern int __fxstatat64(int,int,const char *,struct stat64 *,int);
#define SETTINGS_FSTAT(fd,st) __fxstat64(3,(fd),(st))
#define SETTINGS_FSTATAT(fd,path,st,flags) __fxstatat64(3,(fd),(path),(st),(flags))
#else
#define SETTINGS_STAT struct stat
#define SETTINGS_FSTAT(fd,st) fstat((fd),(st))
#define SETTINGS_FSTATAT(fd,path,st,flags) fstatat((fd),(path),(st),(flags))
#endif
#ifndef LX06_SETTINGS_DIR
#define LX06_SETTINGS_DIR "/data/lx06/script-settings"
#endif
#define SETTINGS_CAP 8192
static int settings_name(const char *name) {
    size_t n=strlen(name);if(!n||n>64||name[0]=='.')return 0;
    for(const char*p=name;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("._-",*p)))return 0;
    return 1;
}
static int settings_dir(int create) {
    if(create&&mkdir(LX06_SETTINGS_DIR,0700)&&errno!=EEXIST)return -1;
    int d=open(LX06_SETTINGS_DIR,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(d<0)return -1;
    SETTINGS_STAT st;
    if(SETTINGS_FSTAT(d,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0700){close(d);errno=EPERM;return -1;}
    return d;
}
static int settings_read(int dir,const char*name,char out[SETTINGS_CAP+1]) {
    char file[80];if(!settings_name(name))return -1;
    snprintf(file,sizeof(file),"%s.json",name);
    int f=openat(dir,file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(f<0)return errno==ENOENT?0:-1;
    SETTINGS_STAT st;int rc=-1;
    if(!SETTINGS_FSTAT(f,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&st.st_nlink==1&&
       (st.st_mode&0777)==0600&&st.st_size>0&&st.st_size<=SETTINGS_CAP) {
        size_t n=0;ssize_t z;
        while(n<(size_t)st.st_size&&(z=read(f,out+n,(size_t)st.st_size-n))>0)n+=(size_t)z;
        if(n==(size_t)st.st_size){out[n]=0;rc=1;}
    }
    close(f);return rc;
}

/* The existing settings lock also serializes cooperating source writers.
 * Reopen the named entries before each commit: an unlinked/replaced lock or
 * renamed store must not grant authority over a new pathname. No LOCK_UN;
 * closing our reference preserves any same-open-description retained owner. */
static int settings_lock_current(int dir,int lock) {
    SETTINGS_STAT store,named_store,held,named;
    int current=-1,entry=-1,rc=-1;
    if(dir<0||lock<0||SETTINGS_FSTAT(dir,&store)||SETTINGS_FSTAT(lock,&held))goto done;
    if(!S_ISDIR(store.st_mode)||store.st_uid!=getuid()||(store.st_mode&0777)!=0700||
       !S_ISREG(held.st_mode)||held.st_uid!=getuid()||held.st_nlink!=1||(held.st_mode&0777)!=0600)goto done;
    current=settings_dir(0);
    if(current<0||SETTINGS_FSTAT(current,&named_store)||
       store.st_dev!=named_store.st_dev||store.st_ino!=named_store.st_ino)goto done;
    entry=openat(dir,".lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(entry<0||SETTINGS_FSTAT(entry,&named)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||
       held.st_mode!=named.st_mode||held.st_uid!=named.st_uid||held.st_nlink!=named.st_nlink)goto done;
    if(flock(lock,LOCK_EX|LOCK_NB))goto done;
    rc=0;
done:
    if(entry>=0)close(entry);
    if(current>=0)close(current);
    if(rc)errno=EBUSY;
    return rc;
}
static int settings_lock_acquire(int dir) {
    if(dir<0){errno=EINVAL;return -1;}
    int lock=openat(dir,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(lock<0)return -1;
    if(settings_lock_current(dir,lock)){int e=errno;close(lock);errno=e;return -1;}
    return lock;
}
#endif
