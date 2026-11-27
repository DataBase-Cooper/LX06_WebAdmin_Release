/* Shared session ownership for web dispatch, native wake and script runners.
 * Never unlink the lock inode. Verify /proc start ticks before signalling a PID.
 */
#ifndef LX06_SESSION_OWNER_H
#define LX06_SESSION_OWNER_H
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __xstat64(int,const char *,struct stat64 *);
#define LX06_SESSION_STATBUF struct stat64
#define LX06_SESSION_STAT(path,st) __xstat64(3,(path),(st))
#else
#define LX06_SESSION_STATBUF struct stat
#define LX06_SESSION_STAT(path,st) stat((path),(st))
#endif
#ifndef LX06_SESSION_FILE
#define LX06_SESSION_FILE "/tmp/lx06-session.owner"
#endif
struct lx06_session_record {
    uint32_t magic, supervisor, group, reserved;
    uint64_t supervisor_ticks, group_ticks;
};
#define LX06_SESSION_MAGIC 0x3153584cU
#define LX06_SESSION_NAME_SIZE 64
static inline int lx06_session_name_valid(const char *name) {
    size_t n=strlen(name);
    return n>0 && n<LX06_SESSION_NAME_SIZE &&
        strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")==n;
}
static uint64_t lx06_process_ticks(unsigned pid) {
    char path[64], data[1024];
    snprintf(path,sizeof(path),"/proc/%u/stat",pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0) return 0;
    ssize_t n=read(fd,data,sizeof(data)-1); close(fd);
    if(n<=0) return 0;
    data[n]=0; char *p=strrchr(data,')');
    if(!p) return 0;
    p+=2;
    /* p starts at field 3; field 22 is the immutable process birth tick. */
    for(int field=3;field<22;field++) {
        p=strchr(p,' '); if(!p) return 0; ++p;
    }
    uint64_t ticks=0;
    while(*p>='0' && *p<='9') ticks=ticks*10+(unsigned)(*p++-'0');
    return ticks;
}
/* A session lease is inherited by the controlled script/reader/player. If its
 * supervisor AND group leader die, the lock still excludes a new session. A
 * surviving holder can be identified without trusting a recycled PGID. */
static int lx06_orphan_signal(const struct lx06_session_record *record,int sig) {
    if(record->reserved<3 || record->reserved>1024) return 0;
    LX06_SESSION_STATBUF owner;
    if(LX06_SESSION_STAT(LX06_SESSION_FILE,&owner)) return 0;
    DIR *dir=opendir("/proc"); if(!dir) return -1;
    struct dirent *entry; int count=0;
    while((entry=readdir(dir))) {
        unsigned pid=0;
        const char *digit=entry->d_name;
        while(*digit>='0' && *digit<='9' && pid<10000000)
            pid=pid*10+(unsigned)(*digit++-'0');
        if(*digit) continue;
        if(pid<2 || pid==(unsigned)getpid()) continue;
        char path[96]; LX06_SESSION_STATBUF lease;
        snprintf(path,sizeof(path),"/proc/%u/fd/%u",pid,record->reserved);
        if(LX06_SESSION_STAT(path,&lease) || lease.st_dev!=owner.st_dev || lease.st_ino!=owner.st_ino) continue;
        uint64_t ticks=lx06_process_ticks(pid);
        if(!ticks || ticks<record->group_ticks) continue;
        ++count;
        if(sig && lx06_process_ticks(pid)==ticks) (void)kill((pid_t)pid,sig);
    }
    closedir(dir); return count;
}
static inline int lx06_session_identity(struct lx06_session_record *record,char *name) {
    memset(record,0,sizeof(*record));
    if(name) name[0]=0;
    int fd=open(LX06_SESSION_FILE,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return errno==ENOENT ? 0 : 1;
    /* Status readers can share the idle observation with capture. Real
     * audio and admission reservations still hold an exclusive lock. */
    int held=flock(fd,LOCK_SH|LOCK_NB)!=0;
    ssize_t n=pread(fd,record,sizeof(*record),0);
    char suffix[LX06_SESSION_NAME_SIZE]={0};
    ssize_t extra=name ? pread(fd,suffix,sizeof(suffix),sizeof(*record)) : 0;
    close(fd);
    if(n!=(ssize_t)sizeof(*record) || record->magic!=LX06_SESSION_MAGIC) {
        memset(record,0,sizeof(*record)); return held;
    }
    /* The first 32 bytes stay compatible with the deployed native bridge.
     * Only a live, lease-holding supervisor can supply a route identity. */
    if(name && held && extra==sizeof(suffix) && suffix[sizeof(suffix)-1]==0 &&
       lx06_session_name_valid(suffix) && record->supervisor>1 &&
       record->supervisor_ticks && lx06_process_ticks(record->supervisor)==record->supervisor_ticks)
        memcpy(name,suffix,sizeof(suffix));
    /* If a supervisor was killed, a still-identical group leader remains an
     * owned orphan. Refuse a competing session until that group is stopped. */
    return held || (record->group>1 && record->group_ticks &&
                    lx06_process_ticks(record->group)==record->group_ticks);
}
static int lx06_session_read(struct lx06_session_record *record) {
    return lx06_session_identity(record,NULL);
}
static int lx06_session_busy(void) {
    struct lx06_session_record record;
    return lx06_session_read(&record);
}
/* Acquire the same exclusive reservation as session-run and check recorded
 * surviving owners while holding it. The caller owns the descriptor. */
static inline int lx06_session_lock_idle_at(const char *path,int operation,int create_if_missing) {
    struct lx06_session_record record;
    int saved;
    int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW|(create_if_missing ? O_CREAT : 0),0600);
    if(fd<0) return -1;
    if(flock(fd,operation|LOCK_NB)) goto failed;
    ssize_t n;
    do { n=pread(fd,&record,sizeof(record),0); } while(n<0 && errno==EINTR);
    if(n<0) goto failed;
    if(n && (n!=(ssize_t)sizeof(record) || record.magic!=LX06_SESSION_MAGIC)) {
        errno=EINVAL; goto failed;
    }
    if(n && record.group>1 && record.group_ticks &&
       lx06_process_ticks(record.group)==record.group_ticks) {
        errno=EBUSY; goto failed;
    }
    return fd;
failed:
    saved=errno;
    close(fd);
    errno=saved;
    return -1;
}
static inline int lx06_session_lock_idle(int operation) {
    return lx06_session_lock_idle_at(LX06_SESSION_FILE,operation,1);
}
static inline int lx06_session_take_idle(void) {
    return lx06_session_lock_idle(LOCK_EX);
}
static inline int lx06_session_preempt_expected(const struct lx06_session_record *expected) {
    struct lx06_session_record record;
    if(!lx06_session_read(&record)) return 0;
    if(expected && memcmp(expected,&record,sizeof(record))) return -1;
    if(record.supervisor>1 && record.supervisor_ticks &&
       lx06_process_ticks(record.supervisor)==record.supervisor_ticks)
        return kill((pid_t)record.supervisor,SIGUSR1)==0 ? 1 : -1;
    /* No supervisor can do a graceful bounded shutdown now. Kill only holders
     * of this session lease; unrelated processes with recycled IDs are safe. */
    int killed=lx06_orphan_signal(&record,SIGKILL);
    if(record.group>1 && record.group_ticks &&
       lx06_process_ticks(record.group)==record.group_ticks)
        return kill(-(pid_t)record.group,SIGKILL)==0 ? 1 : -1;
    return killed>0 ? 1 : -1;
}
static int lx06_session_preempt(void) {
    return lx06_session_preempt_expected(NULL);
}
#endif
