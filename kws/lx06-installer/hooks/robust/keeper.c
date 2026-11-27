/* 原生单进程 keeper：不重试、不重启、不执行不可信卸载/回滚。
 * RSS 是定时软采样，不是 cgroup 硬隔离；不承诺内核、tmpfs、socket 永不耗尽。
 */
#define _GNU_SOURCE
#include "keeper_client.h"
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifndef LK_STATE_DIR
#define LK_STATE_DIR "/data/lx06/keeper"
#endif
#ifndef LK_RUN_DIR
/* OpenWrt 的 /var 是符号链接（/var->/tmp），open_dir 逐分量 O_NOFOLLOW 会 ENOTDIR；
 * 直接用真实目录 /tmp/run（S50 dropbear 已在用），bind/connect 不受影响。 */
#define LK_RUN_DIR "/tmp/run/lx06-keeper"
#endif
#ifndef LK_USER_DIR
#define LK_USER_DIR "/data/lx06/user"
#endif
#ifndef LK_LOG_DIR
#define LK_LOG_DIR "/tmp/lx06-keeper-logs"
#endif
#ifndef LK_UPLOAD_DIR
#define LK_UPLOAD_DIR "/tmp/lx06-uploads"
#endif
#ifndef LK_BK_DIR
#define LK_BK_DIR "/data/lx06/.bk"
#endif
#ifndef LK_MEMINFO
#define LK_MEMINFO "/proc/meminfo"
#endif
#define UID_BASE LK_UID_BASE
#define JOB_FILE_CAP 262144
#define NCLIENT 16
#define VM_DEFAULT (64UL * 1024 * 1024)
#define VM_WIDE (256UL * 1024 * 1024)
struct procinfo { pid_t pid; uid_t uid; pid_t ppid; uint64_t ticks, rss; char state; };
struct job {
    int used, stopping, reaped, status, pipefd, execfd, client, logfd, exec_ready;
    int64_t kill_at, exec_until, until;
    pid_t pid; uid_t uid; uint64_t ticks, rss; unsigned members;
    char name[LK_NAME_LEN];
};
struct client { int fd, pending; int64_t until; };
static struct job jobs[LK_MAX_SLOTS];
/* 稳定持久 UID 租约包含已停止应用，FORGET 清净目录后才释放。 */
static char slots[LK_MAX_SLOTS][LK_NAME_LEN];
static int slot_root[LK_MAX_SLOTS]; /* root 执行槽（行尾 " +" 持久化，LKS1 行格式兼容旧读法） */
static struct client clients[NCLIENT];
static int statefd=-1, runfd=-1, userfd=-1, logdirfd=-1, uploadfd=-1;
static int listenfd=-1, lockfd=-1, safe_mode, active_state, disk_safe=-1, disk_active=-1;
static int persist_broken;
static uint64_t budget, reserve_kb;
static char reason[160], boot_id[80];
static volatile sig_atomic_t quitting;
/* ---- glibc 统一 syscall trampoline 拦截（白名单）----
 * 静态 glibc 发出的多参 syscall 全经 __libc_do_syscall（r12=号, r0-r5=参,
 * r7 由 push/pop 保持）。全日志实测 oops 号仅 397(statx)×318、398(rseq)×3；
 * 403(clock_gettime64)/422(futex_time64)/436(close_range) 为潜在号一并拦截。
 * 注意不能一刀切 ≥386：386-396 区间（membarrier/mlock2 等）内核有实现且
 * glibc 启动 TLS 分配依赖，误杀会致 "Cannot allocate TLS block" 直接死亡。
 * 这里同名遮蔽 glibc 符号（静态链接本对象先于 libc.a），命中白名单直接返回
 * -ENOSYS 不进内核（glibc 按设计走旧号回退），其余原样透传。 */
#if defined(__arm__)
__attribute__((naked, used))
long __libc_do_syscall(long n, long a0, long a1, long a2,
                       long a3, long a4, long a5) {
    (void)n; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    __asm__ __volatile__ (
        "push {r7, fp, ip, lr}\n"
        "mov fp, r12\n"        /* 先存 syscall 号：ip(r12) 马上要当 scratch 用 */
        "movw ip, #397\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #398\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #403\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #422\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #436\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "mov r7, fp\n"
        "svc #0\n"
        "pop {r7, fp, ip, pc}\n"
        "1:\n"
        "mvn r0, #37\n"       /* r0 = ~37 = 0xFFFFFFDA = -ENOSYS(-38)。
                                  glibc 包装层见 r0<0 置 errno=38 返回 -1，
                                  走旧号回退；返回 +38 会被当作成功踩空指针 */
        "pop {r7, fp, ip, pc}\n"
        : : : "memory");
}
#else
/* 非 ARM（x86 单测构建）：无 iplt 陷阱，直接透传；白名单拦截不适用。 */
__attribute__((used))
long __libc_do_syscall(long n, long a0, long a1, long a2,
                       long a3, long a4, long a5) {
    return (long)syscall(n, a0, a1, a2, a3, a4, a5);
}
#endif
/* ---- stat 族与目录读走裸 syscall（4.9.61 无 time64 号）----
 * 真机探针（.166）实测：glibc 的 fstat()/lstat() 实际走 64 位号并填
 * 64 位布局，32 位号(107/108)填的是内核经典 32 位 struct stat（16 位
 * mode/nlink/uid 紧凑布局），与 _TIME_BITS=32 的 glibc struct stat
 * 布局不一致，直接读会得脏值。故统一走 64 位变体：
 *   lstat64=196, fstat64=197, fstatat64=327, getdents64=217
 * 先填 stat64，再把代码用到的字段拷回应用侧 struct stat。
 * 注意：glibc syscall() 出错时返回 -1 并已设置 errno（真机探针证实），
 * 不得再用 errno=-r 覆盖——r 恒为 -1，会把真实 errno 改成 EPERM(1)。
 * 非 ARM（x86 单测构建）内核 stat 布局即 64 位，直接走 libc 包装。 */
struct lk_dent64 { uint64_t d_ino; uint64_t d_off; unsigned short d_reclen; char d_type; char d_name[1]; };
#if defined(__arm__)
static void lk_stat64_to_stat(struct stat *s,const struct stat64 *s64) {
    s->st_dev=s64->st_dev; s->st_ino=(ino_t)s64->st_ino;
    s->st_mode=s64->st_mode; s->st_nlink=(nlink_t)s64->st_nlink;
    s->st_uid=(uid_t)s64->st_uid; s->st_gid=(gid_t)s64->st_gid;
    s->st_rdev=s64->st_rdev; s->st_size=(off_t)s64->st_size;
    s->st_blksize=s64->st_blksize; s->st_blocks=(blkcnt_t)s64->st_blocks;
}
static int lk_fstat(int fd,struct stat *s) {
    struct stat64 s64;
    if (syscall(197,fd,&s64)<0) return -1;
    lk_stat64_to_stat(s,&s64);
    return 0;
}
static int lk_fstatat(int d,const char *p,struct stat *s,int fl) {
    struct stat64 s64;
    if (syscall(327,d,p,&s64,fl)<0) return -1;
    lk_stat64_to_stat(s,&s64);
    return 0;
}
static int lk_lstat(const char *p,struct stat *s) {
    struct stat64 s64;
    if (syscall(196,p,&s64)<0) return -1;
    lk_stat64_to_stat(s,&s64);
    return 0;
}
static int lk_getdents(int fd,void *buf,size_t n) {
    long r=syscall(217,fd,buf,n);
    if (r<0) return -1;
    return (int)r;
}
#else
static int lk_fstat(int fd,struct stat *s) { return fstat(fd,s); }
static int lk_fstatat(int d,const char *p,struct stat *s,int fl) { return fstatat(d,p,s,fl); }
static int lk_lstat(const char *p,struct stat *s) { return lstat(p,s); }
static int lk_getdents(int fd,void *buf,size_t n) {
    long r=syscall(SYS_getdents64,fd,buf,n);
    if (r<0) return -1;
    return (int)r;
}
#endif
#define LX06_LOG_FSTAT lk_fstat
#include "bounded-log.h"
#define LX06_POLICY_STAT struct stat
#define LX06_POLICY_FSTAT lk_fstat
#include "script-policy.h"
/* /etc/passwd、/etc/group 裸读：静态 glibc 2.41 的 getpwuid/getgrgid 走 stdio
 * fopen → 64 位 fstat → oops。这里只需验证“该 UID/GID 条目存在”，直接解析
 * （本机 /etc 为普通文件，无 NSS 模块）。返回 0=存在 1=不存在 -1=读错。 */
static int raw_uid_exists(uid_t uid) {
    char b[8192]; int fd; size_t tot=0;
    fd=open("/etc/passwd",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (;;) {
        ssize_t n=read(fd,b+tot,sizeof(b)-1-tot);
        if (n<0) { if (errno==EINTR) continue; close(fd); return -1; }
        if (!n) break;
        tot+=(size_t)n;
        if (tot==sizeof(b)-1) break;
    }
    b[tot]=0; close(fd);
    for (char *line=b; *line; ) {
        char *nl=strchr(line,'\n'); if (nl)*nl=0;
        { unsigned u=0; char *f=strchr(line,':'); int found=0;
          if (f) f=strchr(f+1,':');
          if (f) found=(sscanf(f+1,"%u",&u)==1&&(uid_t)u==uid);
          if (found) return 0;
        }
        if (!nl) break;
        line=nl+1;
    }
    return 1;
}
static int raw_gid_exists(gid_t gid) {
    char b[8192]; int fd; size_t tot=0;
    fd=open("/etc/group",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (;;) {
        ssize_t n=read(fd,b+tot,sizeof(b)-1-tot);
        if (n<0) { if (errno==EINTR) continue; close(fd); return -1; }
        if (!n) break;
        tot+=(size_t)n;
        if (tot==sizeof(b)-1) break;
    }
    b[tot]=0; close(fd);
    for (char *line=b; *line; ) {
        char *nl=strchr(line,'\n'); if (nl)*nl=0;
        { unsigned g=0; char *f=strchr(line,':'); int found=0;
          if (f) f=strchr(f+1,':');
          if (f) f=strchr(f+1,':');
          if (f) found=(sscanf(f+1,"%u",&g)==1&&(gid_t)g==gid);
          if (found) return 0;
        }
        if (!nl) break;
        line=nl+1;
    }
    return 1;
}
/* 生命周期内固定总预算，不按应用分别取 1/4，不随 available 下降重算。 */
uint64_t lk_budget_kb(uint64_t total, uint64_t available) {
    uint64_t reserve=total/8, target=total/4, room;
    if (reserve<16384) reserve=16384;
    room=available>reserve ? available-reserve : 0;
    return room<target ? room : target;
}
static int valid_name(const char *s) {
    size_t n=strnlen(s,LK_NAME_LEN), i;
    if (!n || n>=LK_NAME_LEN || s[0]=='.' || !strcmp(s,"-")) return 0;
    for (i=0;i<n;++i)
        if (!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
              (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.')) return 0;
    return 1;
}
static int write_all(int fd,const void *p,size_t n) {
    const char *s=p;
    while (n) { ssize_t k=write(fd,s,n);
        if (k<0&&errno==EINTR) continue;
        if (k<=0) return -1;
        s+=k; n-=(size_t)k;
    }
    return 0;
}
static int trusted(int fd,mode_t mode,int regular) {
    struct stat s;
    if (lk_fstat(fd,&s)||s.st_uid!=0||(s.st_mode&0777)!=mode||
        (regular?(!S_ISREG(s.st_mode)||s.st_nlink!=1):!S_ISDIR(s.st_mode))) {
        errno=EPERM; return -1;
    }
    return 0;
}
/* 每个分量 O_NOFOLLOW；托管根不能由普通用户预先占位。/tmp 仅可作祖先。 */
static int open_dir(const char *path,mode_t mode,int create) {
    char b[PATH_MAX], *save=NULL, *part;
    int fd,next,first=1;
    const char *relative=path;
    if (path[0]!='/'||strlen(path)>=sizeof(b)) { errno=EINVAL; return -1; }
#if defined(LK_TESTING) && defined(LK_TEST_ANCHOR)
    /* 仅本地测试：DrvFS 不支持 Unix mode，测试在 scratch 下挂有界 tmpfs。
     * 生产编译无此入口；锚点 root:0755 及锚点下面所有分量仍严格检查。 */
    if (!strncmp(path,LK_TEST_ANCHOR,strlen(LK_TEST_ANCHOR))&&path[strlen(LK_TEST_ANCHOR)]=='/') {
        fd=open(LK_TEST_ANCHOR,O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if (fd<0) return -1;
        if (trusted(fd,0755,0)) { close(fd); return -1; }
        relative=path+strlen(LK_TEST_ANCHOR); first=0;
    } else
#endif
    fd=open("/",O_DIRECTORY|O_RDONLY|O_CLOEXEC);
    strcpy(b,relative);
    if (fd<0) return -1;
    part=strtok_r(b,"/",&save);
    while (part) {
        char *after=strtok_r(NULL,"/",&save);
        struct stat s;
        if (!strcmp(part,".")||!strcmp(part,"..")) { close(fd); errno=EINVAL; return -1; }
        next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (next<0&&errno==ENOENT&&create) {
            if (mkdirat(fd,part,after?0755:mode)&&errno!=EEXIST) { close(fd); return -1; }
            next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        }
        close(fd); if (next<0) return -1;
        /* 唯一公共祖先例外是根下面的 /tmp(root:1777)，不能泛化成任意 sticky 目录。 */
        if (lk_fstat(next,&s)||s.st_uid!=0||((s.st_mode&0022)&&
            !(first&&!strcmp(part,"tmp")&&(s.st_mode&07777)==01777))) {
            close(next); errno=EPERM; return -1;
        }
        first=0; fd=next; part=after;
    }
    if (trusted(fd,mode,0)) { close(fd); return -1; }
    return fd;
}
static int read_small(int d,const char *name,char *b,size_t size,mode_t mode) {
    int fd=openat(d,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    ssize_t n;
    if (fd<0) return -1;
    if (trusted(fd,mode,1)) { close(fd); return -1; }
    n=read(fd,b,size); close(fd);
    if (n<0) return -1;
    if ((size_t)n>=size||memchr(b,0,(size_t)n)) { errno=EINVAL; return -1; }
    b[n]=0; return 0;
}
/* 持久状态：文件 fsync -> 原子 rename -> 父目录 fsync。 */
static int atomic_file(int d,const char *name,const char *data,mode_t mode,int durable) {
    char tmp[80]; int fd,e; struct stat s;
    if (!lk_fstatat(d,name,&s,AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(s.st_mode)||s.st_uid!=0||s.st_nlink!=1||(s.st_mode&0022)) { errno=EPERM; return -1; }
    } else if (errno!=ENOENT) return -1;
    snprintf(tmp,sizeof(tmp),".write-%ld",(long)getpid());
    fd=openat(d,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,mode);
    if (fd<0) return -1;
    if (write_all(fd,data,strlen(data))||fchmod(fd,mode)||(durable&&fsync(fd))) goto fail;
    if (close(fd)) { fd=-1; goto fail; } fd=-1;
    if (renameat(d,tmp,d,name)||(durable&&fsync(d))) goto fail;
    return 0;
fail:
    e=errno; if (fd>=0) close(fd); unlinkat(d,tmp,0); errno=e; return -1;
}
static int meminfo(uint64_t *total,uint64_t *available) {
    char b[512]; unsigned long long t=0,a=0,v;
    int gt=0,ga=0,fd,o;
    /* 裸 open/read：glibc fopen → 64 位 fstat → oops（每秒一次）。 */
    fd=open(LK_MEMINFO,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (o=0;o<(int)sizeof(b)-1;) {
        ssize_t n=read(fd,b+o,sizeof(b)-1-(size_t)o);
        if (n<0) { if (errno==EINTR) continue; close(fd); return -1; }
        if (!n) break;
        o+=(int)n;
    }
    close(fd);
    b[o]=0;
    for (char *line=b; *line; ) {
        char *nl=strchr(line,'\n');
        if (nl) *nl=0;
        { char extra;
          if (sscanf(line,"MemTotal: %llu kB %c",&v,&extra)==1) { t=v; gt=1; }
          if (sscanf(line,"MemAvailable: %llu kB %c",&v,&extra)==1) { a=v; ga=1; }
        }
        if (!nl) break;
        line=nl+1;
    }
    if (!gt||!ga||!t||a>t||t>UINT64_MAX/1024) { errno=EIO; return -1; }
    *total=t; *available=a; return 0;
}
static int read_proc(pid_t pid,struct procinfo *p) {
    char path[80],b[4096],*tail,*save=NULL,*tok;
    ssize_t n; int fd,field;
    unsigned real,effective,saved,fsuid;
    /* 使用真实凭据字段，而不是把 /proc inode owner 当作用户态隔离协议。
     * 本地 dumpable=0 回归仍成立；不依赖不同内核的 proc inode 实现细节。 */
    snprintf(path,sizeof(path),"/proc/%ld/status",(long)pid);
    fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return errno==ENOENT||errno==ESRCH?0:-1;
    n=read(fd,b,sizeof(b)-1); close(fd);
    if (n<0) return errno==ENOENT||errno==ESRCH?0:-1;
    if (!n) return 0;
    b[n]=0; tail=strstr(b,"\nUid:");
    if (!tail||sscanf(tail,"\nUid: %u %u %u %u",&real,&effective,&saved,&fsuid)!=4) {
        errno=EIO; return -1;
    }
    p->pid=pid; p->uid=(uid_t)real; p->ppid=0; p->rss=p->ticks=0;
    snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
    fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return errno==ENOENT||errno==ESRCH?0:-1;
    n=read(fd,b,sizeof(b)-1); close(fd);
    if (n<0) return errno==ENOENT||errno==ESRCH?0:-1;
    if (!n) return 0;
    b[n]=0; tail=strrchr(b,')');
    if (!tail||tail[1]!=' ') { errno=EIO; return -1; }
    p->state=tail[2];
    for (field=3,tok=strtok_r(tail+2," ",&save);tok;++field,tok=strtok_r(NULL," ",&save)) {
        /* stat 字段（以 (comm) 为 f2 起算）：22=starttime(唯一身份, pid+starttime
         * 是标准进程身份, pid1 可为 0，不得要求非零)；24=rss(页)。
         * 历史坑：曾把 20(=num_threads)当身份、22(=starttime)当 rss——
         * starttime 被当成页数换算后单进程“rss”可达数 GB，job 一起来就
         * 超过内存预算被杀；rss 真身在 24（VmRSS 已交叉验证）。
         * 内核线程 rss=0 合法：pages>0 才累加，不得判 EIO。 */
        if (field==4) p->ppid=(pid_t)strtol(tok,NULL,10);
        if (field==22) p->ticks=strtoull(tok,NULL,10);
        if (field==24) { long long pages=strtoll(tok,NULL,10);
            if (pages>0) p->rss=(uint64_t)pages*(uint64_t)sysconf(_SC_PAGESIZE)/1024;
            break;
        }
    }
    if (field<24) { errno=EIO; return -1; }
    return 1;
}
static void identity_signal(const struct procinfo *p,int sig) {
    struct procinfo now;
    /* 不只用 PID/PGID：再次核对 UID+starttime；setsid 不能逃避 UID 巡检。 */
    if (read_proc(p->pid,&now)==1&&now.uid==p->uid&&now.ticks==p->ticks) (void)kill(p->pid,sig);
}
static void signal_launcher(struct job *j,int sig) {
    struct procinfo p;
    /* 启动早期还没切 UID，也必须能取消；只允许精确的本 daemon 子 PID+ticks，
     * 绝不能因“清理 reserved UID”扩大到其他 root 进程。 */
    if (!j->reaped&&j->pid>0&&j->ticks&&read_proc(j->pid,&p)==1&&p.ticks==j->ticks&&
        (p.uid==j->uid||(!j->exec_ready&&p.uid==0))) identity_signal(&p,sig);
}
/* RSS accounting only: root jobs share UID with stock engines. Follow bounded
 * ancestry to a live, birth-tick-verified launcher; never signal by this rule.
 * Adopted orphans have no launcher ancestor and remain in ext_refresh(). */
static int root_owner_slot(const struct procinfo *process) {
    struct procinfo p=*process;
    int roots=0;
    for(int i=0;i<LK_MAX_SLOTS;i++) if(jobs[i].used&&jobs[i].uid==0&&!jobs[i].reaped) roots=1;
    if(!roots)return -1;
    for(unsigned depth=0;depth<64;depth++) {
        for(int i=0;i<LK_MAX_SLOTS;i++) {
            const struct job *j=&jobs[i];
            if(j->used&&j->uid==0&&!j->reaped&&j->pid==p.pid&&j->ticks==p.ticks)return i;
        }
        if(p.ppid<=1||p.ppid==getpid()||p.ppid==p.pid)return -1;
        if(read_proc(p.ppid,&p)!=1)return -1;
    }
    return -1;
}
static int scan(int sig,uid_t only,unsigned *count,uint64_t *rss) {
    int fd,err=0,i,n; char *end; long id; struct procinfo p;
    char db[2048] __attribute__((aligned(8)));
    /* 裸 open + getdents64：glibc opendir 内部 fstatat → 64 位时间号 → oops
     * （每秒一次）。4.9.61 实现 getdents64(217)。 */
    if (count) *count=0;
    if (rss) *rss=0;
    if (!sig&&!only) for (i=0;i<LK_MAX_SLOTS;++i) { jobs[i].members=0; jobs[i].rss=0; }
    fd=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (;;) {
        n=lk_getdents(fd,db,sizeof db);
        if (n<0) { err=1; break; }
        if (!n) break;
        for (size_t o=0;o<(size_t)n;) {
            struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
            o+=(size_t)de->d_reclen;
            if (de->d_name[0]<'0'||de->d_name[0]>'9') continue;
            id=strtol(de->d_name,&end,10);
            if (*end||id<=0||id>INT_MAX) continue;
            { int r=read_proc((pid_t)id,&p);
              if (r<0) { err=1; continue; }
              if (!r) continue;
              if (p.uid<UID_BASE||p.uid>=UID_BASE+LK_MAX_SLOTS) {
                  if(!sig&&!only&&(i=root_owner_slot(&p))>=0) {
                      if(count)++*count;
                      if(rss)*rss+=p.rss;
                      ++jobs[i].members;jobs[i].rss+=p.rss;
                  }
                  continue;
              }
              if(only&&p.uid!=only)continue;
              if (count) ++*count;
              if (rss) *rss+=p.rss;
              if (!sig&&!only) { i=(int)(p.uid-UID_BASE); ++jobs[i].members; jobs[i].rss+=p.rss; }
              if (sig) identity_signal(&p,sig);
            }
        }
    }
    close(fd);
    if (err) { errno=EIO; return -1; } return 0;
}
static int valid_boot(const char *s);
static int persist_state(void) {
    char b[300];
    if (persist_broken) { errno=EIO; return -1; }
    if (!persist_broken&&disk_safe==safe_mode&&disk_active==active_state) return 0;
    /* LK2 携带 boot_id：跨 boot 可证明旧 daemon 已死，stale active 可自动清。
       boot_id 不可读时退回 LK1（保守：active=1 仍需人工 recover）。 */
    if (valid_boot(boot_id))
        snprintf(b,sizeof(b),"LK2 %d %d %.*s\n%s\n",safe_mode,active_state,36,boot_id,reason);
    else
        snprintf(b,sizeof(b),"LK1 %d %d\n%s\n",safe_mode,active_state,reason);
    if (atomic_file(statefd,"state",b,0600,1)) {
        persist_broken=1; safe_mode=1; snprintf(reason,sizeof(reason),"state-write-failed: manual repair required"); return -1;
    }
    disk_safe=safe_mode; disk_active=active_state; persist_broken=0; return 0;
}
static void latch_safe(const char *why) {
    int i;
    if (!safe_mode) { safe_mode=1; snprintf(reason,sizeof(reason),"%s",why); }
    /* 先持久记录再杀；写入失败也内存锁死并杀，不能因为写盘失败重新放行。 */
    (void)persist_state();
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) {
        jobs[i].stopping=1; jobs[i].kill_at=0; signal_launcher(&jobs[i],SIGKILL);
    }
    (void)scan(SIGKILL,0,NULL,NULL);
}
static void load_state(void) {
    char b[300],*end,*line; long s,a; int isv2,i;
    if (read_small(statefd,"state",b,sizeof(b)-1,0600)) {
        if (errno!=ENOENT) { safe_mode=1; snprintf(reason,sizeof(reason),"state-unreadable-or-unsafe"); }
        else {
            struct stat st; int evidence=0;
            if (!lk_fstatat(statefd,"slots",&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT) evidence=1;
            if (!lk_fstatat(statefd,"auto-boot",&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT) evidence=1;
            /* 只有全新安装允许无 state；已有注册/boot latch 时缺失不是“干净启动”。 */
            if (evidence) { safe_mode=1; snprintf(reason,sizeof(reason),"state-missing-with-existing-registry"); }
        }
    } else {
        isv2=!strncmp(b,"LK2 ",4);
        if (!isv2&&strncmp(b,"LK1 ",4)) goto corrupt;
        errno=0; s=strtol(b+4,&end,10);
        if (errno||*end!=' '||(s!=0&&s!=1)) goto corrupt;
        a=strtol(end+1,&end,10);
        if (errno||(a!=0&&a!=1)) goto corrupt;
        if (isv2) {
            /* LK2：第三字段=写入时的 boot_id（36 字符，无换行）。 */
            if (*end!=' ') goto corrupt;
            line=end+1; end=strchr(line,'\n');
            if (!end||(size_t)(end-line)!=36) goto corrupt;
            for (i=0;i<36;++i) {
                char c=line[i];
                if (i==8||i==13||i==18||i==23) { if (c!='-') goto corrupt; }
                else if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) goto corrupt;
            }
            *end=0;
            line=end+1; end=strchr(line,'\n'); /* 再取 reason 行 */
        } else {
            if (*end!='\n') goto corrupt;
            line=end+1; end=strchr(line,'\n');
        }
        if (!end||end[1]||(size_t)(end-line)>=sizeof(reason)) goto corrupt;
        *end=0; snprintf(reason,sizeof(reason),"%s",line);
        safe_mode=(int)s; active_state=(int)a; disk_safe=(int)s; disk_active=(int)a;
        if (a || s) {
            /* active=1 只在“同一个 boot 内 daemon 复活”时才危险（两个活 daemon）；
               safe=1（latch 后已 idle，a=0）同样可能跨 boot 残留：旧状态会让新 boot
               的 start 全部 ECANCELED，用户代码永久不运行（2026-10-19 .166 实证：
               9c35ffe9 latch 残留卡死 80f3bfdf）。跨 boot（reboot -f / 断电）旧 daemon
               必死：按 boot_id 证明后自动清，免人工 recover。LK2 用 state 内 boot_id；
               LK1 旧格式退回 auto-boot latch 作 boot 标记；boot_id 自身不可读则保守
               保留人工恢复。 */
            int stale_newboot=0;
            char cur[40];
            if (valid_boot(boot_id)) {
                memcpy(cur,boot_id,36); cur[36]=0;
                if (isv2) {
                    stale_newboot = strcmp(line-37,cur)!=0; /* line 已指 reason 起点，前 36 字符=stored bootid */
                } else {
                    char oldb[128];
                    if (read_small(statefd,"auto-boot",oldb,sizeof(oldb)-1,0600)==0)
                        stale_newboot = strcmp(oldb,cur)!=0 && strcmp(oldb,boot_id)!=0;
                    else if (errno==ENOENT) stale_newboot=1; /* 无 latch=全新，视为新 boot */
                }
            }
            if (!stale_newboot) {
                /* 同 boot daemon 复活：保留原 latch 与 reason（锁文件已保证单 daemon；
                   a=1&&s=0 时旧行为保守置 previous-daemon-active）。 */
                if (a && !s) { safe_mode=1; disk_safe=1; snprintf(reason,sizeof(reason),"previous-daemon-active: explicit recover required"); }
            } else {
                /* 跨 boot：safe/active 双双 stale，一并自动清。本 boot 用户代码若仍坏，
                   会立刻重新 latch，证据落盘可查。 */
                s=0; a=0; safe_mode=0; active_state=0; disk_safe=0; disk_active=0;
                snprintf(reason,sizeof(reason),"auto-cleared: stale state from previous boot");
            }
        }
    }
    (void)persist_state(); return;
corrupt:
    safe_mode=1; snprintf(reason,sizeof(reason),"state-corrupt"); (void)persist_state();
}
/* 所有 slot 注册变更原子持久化，失败不再放行 START。 */
static int save_slots(void) {
    char b[LK_MAX_SLOTS * (LK_NAME_LEN + 16)]; size_t pos=0; int i;
    /* Retain the legacy on-disk shape while upper identities are unused.
     * In-place candidate testing can then reboot into the old ROM safely. */
    int entries=LK_MAX_JOBS;
    for(i=LK_MAX_JOBS;i<LK_MAX_SLOTS;i++)if(slots[i][0])entries=LK_MAX_SLOTS;
    pos+=(size_t)snprintf(b,sizeof(b),"LKS1\n");
    for (i=0;i<entries;++i)
        pos+=(size_t)snprintf(b+pos,sizeof(b)-pos,"%d %s%s\n",i,slots[i][0]?slots[i]:"-",slot_root[i]?" +":"");
    if (atomic_file(statefd,"slots",b,0600,1)) {
        latch_safe("slot-registry-write-failed"); persist_broken=1; return -1;
    }
    return 0;
}
/* 用户工作区是否干净：每个条目都必须是已注册槽位目录且 UID 匹配。
   裸 getdents64 直接扫 userfd（fdopendir/readdir 内部 fstatat → oops）。
   load_slots（首启判坏）与孤儿清理路径（forget/LK_RECOVER 释放 persist 锁）共用。 */
static int workspace_dirty(void) {
    char db[1024] __attribute__((aligned(8)));
    int df=dup(userfd),dirty=1;
    if (df<0) return 1;
    /* userfd 的目录位置与主循环共享、不保证在头部：从旧位置（尾部）起
       getdents 立即返回 0 会误判“干净”。先 rewind 再扫，位置无关。 */
    if (lseek(df,0,SEEK_SET)<0) { close(df); return 1; }
    for (;;) {
        int n=lk_getdents(df,db,sizeof db);
        if (n<0) break;
        if (!n) { dirty=0; break; }
        for (size_t o=0;o<(size_t)n;) {
            struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
            struct stat st;
            size_t next=o+(size_t)de->d_reclen;
            int i;
            if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) { o=next; continue; }
            for (i=0;i<LK_MAX_SLOTS;++i) if (!strcmp(slots[i],de->d_name)) break;
            if (i==LK_MAX_SLOTS||lk_fstatat(df,de->d_name,&st,AT_SYMLINK_NOFOLLOW)||
                !S_ISDIR(st.st_mode)||(st.st_uid!=0&&st.st_uid!=(uid_t)(UID_BASE+i))) { close(df); return 1; }
            o=next;
        }
    }
    close(df);
    return dirty;
}
/* 工作区已干净时释放 persist 锁（孤儿目录清理后调用）。
   写失败由 persist_state() 重新上锁，下次删除/解除时再试。 */
static int try_clear_persist(void) {
    if (!persist_broken) return 0;
    if (workspace_dirty()) return -1;
    persist_broken=0; safe_mode=0; active_state=0;
    disk_safe=1; /* 强制重写：磁盘旧状态仍是 safe=1 */
    snprintf(reason,sizeof(reason),"%s","auto-cleared: orphan workspace cleaned");
    return persist_state();
}
static void load_slots(void) {
    char b[LK_MAX_SLOTS * (LK_NAME_LEN + 16)],name[LK_NAME_LEN],*p; int i,k,n;
    if (read_small(statefd,"slots",b,sizeof(b)-1,0600)) {
        /* 元数据漂移自愈（2026-10-20 .166 实证）：slots 被 shell 重定向手工
           修复后是 umask 0644，trusted() 每启 EPERM → slots 清空 → 工作区误判
           dirty → recover 永久 EIO。root 属主、常规文件、nlink=1 时 fchmod
           0600 并重读一次；内容损坏仍由下方 LKS1 解析照常判 corrupt。 */
        int healed=0;
        if (errno==EPERM) {
            struct stat s;
            if (!lk_fstatat(statefd,"slots",&s,AT_SYMLINK_NOFOLLOW) &&
                S_ISREG(s.st_mode)&&s.st_uid==0&&s.st_nlink==1) {
                int fd2=openat(statefd,"slots",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
                if (fd2>=0) {
                    int ok=!fchmod(fd2,0600)&&!lk_fstat(fd2,&s)&&(s.st_mode&0777)==0600;
                    close(fd2);
                    if (ok) healed=!read_small(statefd,"slots",b,sizeof(b)-1,0600);
                }
            }
        }
        if (!healed&&errno==ENOENT) {
            int df,nonempty=0; char db[512] __attribute__((aligned(8)));
            df=dup(userfd);
            if (df<0) goto corrupt;
            for (;;) {
                int n=lk_getdents(df,db,sizeof db);
                if (n<0) { close(df); goto corrupt; }
                if (!n) break;
                for (size_t o=0;o<(size_t)n;++o) {
                    struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
                    o+=(size_t)de->d_reclen-1;
                    if (strcmp(de->d_name,".")&&strcmp(de->d_name,"..")) { nonempty=1; break; }
                }
                if (nonempty) break;
            }
            close(df);
            if (nonempty) goto corrupt;
            (void)save_slots(); return;
        }
        if (!healed) goto corrupt;
    }
    if (strncmp(b,"LKS1\n",5)) goto corrupt;
    p=b+5;
    for (i=0;i<LK_MAX_SLOTS;++i) {
        /* Existing firmware has exactly eight entries. Extend it without
         * changing any existing name/UID mapping; other short files are bad. */
        if(!*p && i==LK_MAX_JOBS) {
            memset(slots+i,0,sizeof(slots[0])*(LK_MAX_SLOTS-i));
            memset(slot_root+i,0,sizeof(slot_root[0])*(LK_MAX_SLOTS-i));
            break;
        }
        n=0;
        /* 行尾可选 " +"=root 执行槽（新 keeper 写入；旧行无后缀=隔离 UID 槽）。 */
        if (sscanf(p,"%d %64s%n",&k,name,&n)!=2||k!=i) goto corrupt;
        slot_root[i]=0;
        if (p[n]==' ') {
            if (p[n+1]!='+'||p[n+2]!='\n') goto corrupt;
            slot_root[i]=1; n+=(int)strlen(" +");
        }
        if (p[n]!='\n') goto corrupt;
        if (strcmp(name,"-")) {
            if (!valid_name(name)) goto corrupt;
            for (k=0;k<i;++k) if (!strcmp(slots[k],name)) goto corrupt;
            strcpy(slots[i],name);
        }
        p+=n+1;
    }
    if (*p) goto corrupt;
    /* 语法合法但 UID/name 与 workspace 不一致也算损坏，不允许静默重分配。 */
    if (workspace_dirty()) goto corrupt;
    return;
corrupt:
    latch_safe("slot-registry-corrupt-or-missing: manual repair required");
    persist_broken=1; /* 不能 recover 后把旧 UID 错配给另一应用。 */
}
static int valid_boot(const char *s) {
    size_t i;
    if (strlen(s)!=37||s[36]!='\n') return 0;
    for (i=0;i<36;++i) {
        if (i==8||i==13||i==18||i==23) { if (s[i]!='-') return 0; }
        else if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return 0;
    }
    return 1;
}
static int auto_latch(void) {
    char old[128];
    if (!valid_boot(boot_id)) { errno=EIO; return -1; }
    if (!read_small(statefd,"auto-boot",old,sizeof(old)-1,0600)) {
        if (!strcmp(old,boot_id)) { errno=EALREADY; return -1; }
        if (!valid_boot(old)) { errno=EIO; return -1; }
    } else if (errno!=ENOENT) return -1;
    /* 全局每 boot 一次，失败也消费，不因 daemon 重启或 recover 清除。 */
    return atomic_file(statefd,"auto-boot",boot_id,0600,1);
}
static void set_reply(struct lk_reply *r) {
    int i;
    memset(r,0,sizeof(*r)); r->magic=LK_MAGIC; r->safe_mode=safe_mode; r->budget_kb=budget;
    snprintf(r->reason,sizeof(r->reason),"%s",reason);
    for (i=0;i<LK_MAX_SLOTS && r->jobs<LK_MAX_JOBS;++i) if (jobs[i].used) {
        struct lk_job_info *j=&r->job[r->jobs++];
        j->pid=jobs[i].pid; j->start_ticks=jobs[i].ticks; j->uid=jobs[i].uid;
        j->rss_kb=jobs[i].rss; j->members=jobs[i].members; r->rss_kb+=j->rss_kb;
        snprintf(j->name,sizeof(j->name),"%s",jobs[i].name);
    }
}
static void close_client(int c) {
    int i;
    if (c<0||clients[c].fd<0) return;
    close(clients[c].fd); clients[c].fd=-1; clients[c].pending=0;
    /* 断连后不能把旧 START 回复发送到复用的 client slot。 */
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&jobs[i].client==c) jobs[i].client=-1;
}
static void answer(int c,int error,struct job *j) {
    struct lk_reply r;
    set_reply(&r); r.error=error;
    if (j&&!error) { r.pid=j->pid; r.start_ticks=j->ticks; }
    if (clients[c].fd>=0) (void)send(clients[c].fd,&r,sizeof(r),MSG_DONTWAIT|MSG_NOSIGNAL);
    close_client(c);
}
static int metadata(const char *name,const char *suffix,const char *s) {
    char leaf[96]; snprintf(leaf,sizeof(leaf),".%s.%s",name,suffix);
    return atomic_file(uploadfd,leaf,s,0600,0);
}
static void runrc(const char *name,int rc) {
    int bk=open_dir(LK_BK_DIR,0700,0),d;
    char b[64]; struct stat st;
    if (bk<0) return; /* 不创建历史备份路径。 */
    d=openat(bk,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); close(bk);
    if (d<0) return;
    /* runid 的内容不必读取；只有已存在的 root 普通单链接文件才允许写回。 */
    if (!trusted(d,0700,0)&&!lk_fstatat(d,"runid",&st,AT_SYMLINK_NOFOLLOW)&&
        S_ISREG(st.st_mode)&&st.st_uid==0&&st.st_nlink==1&&!(st.st_mode&0022)) {
        snprintf(b,sizeof(b),"%d\n",rc);
        if (atomic_file(d,".runrc",b,0600,1)) latch_safe("runrc-write-failed");
    }
    close(d);
}
static void result_files(struct job *j,int rc) {
    char b[64]; snprintf(b,sizeof(b),"%d\n",rc);
    if (metadata(j->name,"rc",b)||metadata(j->name,"st","exited\n")||
        metadata(j->name,"pid","0 0\n")) latch_safe("result-write-failed");
    runrc(j->name,rc);
}
static int prepare_home(struct job *j) {
    int fd; struct stat s;
    if (mkdirat(userfd,j->name,0700)&&errno!=EEXIST) return -1;
    fd=openat(userfd,j->name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    /* 属主必须是 root 或本 job UID；root job 还可接管「本槽位保留 UID」遗留的
     * workspace（非 root 部署后手动 root 运行），随后 chown 回 0；普通 job 接管
     * root workspace 走 st_uid==0 分支。其余属主=未知占用，拒绝。 */
    if (lk_fstat(fd,&s)||(s.st_uid!=0&&s.st_uid!=j->uid&&
        !(j->uid==0&&s.st_uid>=UID_BASE&&s.st_uid<UID_BASE+LK_MAX_SLOTS))||
        fchown(fd,j->uid,j->uid)||fchmod(fd,0700)) { close(fd); errno=EPERM; return -1; }
    return fd;
}
static int hard_limit(int resource,rlim_t v) {
    struct rlimit l={v,v},got;
    if (setrlimit(resource,&l)||getrlimit(resource,&got)||got.rlim_cur!=v||got.rlim_max!=v) return -1;
    return 0;
}
static int lk_dbg_step; /* 调试用：child_exec 当前步骤号，child_fail 写入 job 日志。 */
static void child_fail(int fd) {
    int e=errno?errno:EPERM;
    { char b[96]; int n=snprintf(b,sizeof(b),"CHILDFAIL step=%d errno=%d (%s)\n",lk_dbg_step,e,strerror(e));
      if (n>0) { (void)write(2,b,(size_t)n); (void)fsync(2); } }
    (void)write(fd,&e,sizeof(e)); _exit(126);
}
static int close_other_fds(int keep) {
    int dfd;
    /* 裸 getdents64：opendir("/proc/self/fd") 内部 fstatat → oops。 */
    dfd=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (dfd<0) return -1;
    {
        char db[1024] __attribute__((aligned(8)));
        for (;;) {
            int n=lk_getdents(dfd,db,sizeof db);
            if (n<0) { close(dfd); return -1; }
            if (!n) break;
            for (size_t o=0;o<(size_t)n;) {
                struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
                size_t next=o+(size_t)de->d_reclen;
                char *end; long fd=strtol(de->d_name,&end,10);
                if (!*end&&fd>2&&fd!=keep&&fd!=dfd) close((int)fd);
                o=next;
            }
        }
    }
    close(dfd); return 0;
}
static void child_exec(struct lk_request *q,struct job *j,int home,int output,int errfd,pid_t keeper) {
    int nullfd,oom,i; char cwd[PATH_MAX],homeenv[PATH_MAX+8];
    char *av[LK_ARGC+1],*env[7];
    signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGPIPE,SIG_DFL);
    lk_dbg_step=1;
    if (prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0)||getppid()!=keeper) child_fail(errfd);
#if defined(LK_TESTING) && defined(LK_TEST_SLOW_BEFORE_EXEC)
    /* 有界 0.8s 注入，检查启动超时也能杀还没降 UID 的精确 launcher。 */
    usleep(800000);
#endif
#if defined(LK_TESTING) && defined(LK_TEST_KILL_BEFORE_EXEC)
    /* 仅测试注入：验证 CLOEXEC 管道 EOF 不会把设置前死亡误报成功。 */
    raise(SIGKILL);
#endif
    lk_dbg_step=2; if (setsid()<0||fchdir(home)) child_fail(errfd);
    lk_dbg_step=3;
    nullfd=open("/dev/null",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (nullfd<0||dup2(nullfd,0)<0||dup2(output,1)<0||dup2(output,2)<0) child_fail(errfd);
    /* oom 设置+回读验证必须在 root 期完成：mico 内核 oom_score_adj 为 0400 root，
     * 降权后读回 EPERM（旧代码在降权后验证 → 每个 job 必 126）。 */
    lk_dbg_step=4;
    oom=open("/proc/self/oom_score_adj",O_WRONLY|O_CLOEXEC);
    if (oom<0||write_all(oom,"500\n",4)) child_fail(errfd);
    close(oom);
    lk_dbg_step=5;
    oom=open("/proc/self/oom_score_adj",O_RDONLY|O_CLOEXEC);
    if (oom<0) child_fail(errfd);
    { char b[16]={0}; ssize_t n=read(oom,b,sizeof(b)-1); close(oom);
      if (n<1||strcmp(b,"500\n")) { errno=EPERM; child_fail(errfd); } }
    lk_dbg_step=10; if (close_other_fds(errfd)) child_fail(errfd);
    lk_dbg_step=11; if (prctl(PR_SET_KEEPCAPS,0,0,0,0)) child_fail(errfd);
    /* LK_ROOT：保持 root 身份，跳过 UID/GID 隔离（含保留段冲突检查）；
       其余保护（NO_NEW_PRIVS/rlimits/oom/优先级/日志上限）原样保留。 */
    if (!(q->flags&LK_ROOT)) {
        lk_dbg_step=12; if (setgroups(0,NULL)) child_fail(errfd);
        lk_dbg_step=13; if (setresgid(j->uid,j->uid,j->uid)) child_fail(errfd);
        lk_dbg_step=14; if (setresuid(j->uid,j->uid,j->uid)) child_fail(errfd);
    }
    lk_dbg_step=15; if (getuid()!=j->uid||geteuid()!=j->uid||getgid()!=j->uid||getegid()!=j->uid) { errno=EPERM; child_fail(errfd); }
    if (!(q->flags&LK_ROOT)) {
        lk_dbg_step=16; if (getgroups(0,NULL)!=0) child_fail(errfd);
    }
    lk_dbg_step=17; if (prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0)) child_fail(errfd);
    lk_dbg_step=18; if (getppid()!=keeper) { errno=EPERM; child_fail(errfd); }
    lk_dbg_step=19; if (prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)) child_fail(errfd);
    lk_dbg_step=20; if (prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1) { errno=EPERM; child_fail(errfd); }
    lk_dbg_step=21; if (hard_limit(RLIMIT_NPROC,16)) child_fail(errfd);
    lk_dbg_step=22; if (hard_limit(RLIMIT_NOFILE,32)) child_fail(errfd);
    lk_dbg_step=23; if (hard_limit(RLIMIT_CORE,0)) child_fail(errfd);
    lk_dbg_step=24; if (hard_limit(RLIMIT_FSIZE,JOB_FILE_CAP)) child_fail(errfd);
    /* 写满 JOB_FILE_CAP 时默认 SIGXFSZ 会杀掉脚本（不轮转日志的长跑脚本会
     * 写满→被杀→重启→再写满 死循环）。忽略后 write 返回 EFBIG：
     * 文件仍硬顶 JOB_FILE_CAP（eMMC 保护不变），脚本存活，只丢新数据。 */
    { struct sigaction sa;
      sa.sa_handler=SIG_IGN; sigemptyset(&sa.sa_mask); sa.sa_flags=0;
      if (sigaction(SIGXFSZ,&sa,NULL)) child_fail(errfd); }
    lk_dbg_step=25; if (hard_limit(RLIMIT_AS,q->flags&LK_WIDE_VM?VM_WIDE:VM_DEFAULT)) child_fail(errfd);
    lk_dbg_step=26; if (setpriority(PRIO_PROCESS,0,10)) child_fail(errfd);
    lk_dbg_step=27; if (getpriority(PRIO_PROCESS,0)!=10) { errno=EPERM; child_fail(errfd); }
    lk_dbg_step=60;
    if (snprintf(cwd,sizeof(cwd),"%s/%s",LK_USER_DIR,j->name)>=(int)sizeof(cwd)) {
        errno=ENAMETOOLONG; child_fail(errfd);
    }
    snprintf(homeenv,sizeof(homeenv),"HOME=%s",cwd);
    env[0]=homeenv; env[1]="PATH=/usr/bin:/bin"; env[2]="LANG=C";
    env[3]="USER=lx06"; env[4]="LOGNAME=lx06"; env[5]="TMPDIR=."; env[6]=NULL;
    for (i=0;i<(int)q->argc;++i) av[i]=q->argv[i];
    av[q->argc]=NULL; umask(077);
    /* 两阶段握手：所有限制完成的 ready=0，然后 exec 成功关闭 CLOEXEC 管道。
     * 单独 EOF 也可能是设置前死亡，不能作为成功。 */
    { int ready=0; lk_dbg_step=70; if (write_all(errfd,&ready,sizeof(ready))) child_fail(errfd); }
    lk_dbg_step=80;
    execve(av[0],av,env); child_fail(errfd);
}
/* --- runlog 链接预创建（与 webd app_runlog_path 解析语义一致）---
 * 非 root job uid=6100x 无法在 root 0755 目录（/data/lx06/logs、
 * /tmp/lx06-keeper-logs）创建新文件；链接目标文件若不先由 keeper 预创建
 * 并 chown 到 job UID，脚本 `>> logfile` 即 Permission denied（2026-10-19
 * .166 lg-abs 实证）。best-effort：失败不使 job 启动失败。 */
static void ensure_loglink(struct lk_request *q, const struct job *j) {
    char script[LK_ARG_LEN], path[768] = "", val[512];
    unsigned char buf[8192];
    int fd; ssize_t n;
    struct stat st;
    unsigned off;
    const unsigned char *p;
    if (j->uid == 0) return;
    snprintf(script, sizeof(script), "%s",
             (q->argc > 1 && !strcmp(q->argv[0], "/bin/sh")) ? q->argv[1] : q->argv[0]);
    fd = open(script, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    p = (const unsigned char *)buf;
    /* 找第一个合法 logfile= 行（行首或空白后出现），值到行尾，去空白与
       成对双引号；/ 开头=绝对路径，否则 LK_LOG_DIR/<值>.log。 */
    for (off = 0; off < (unsigned)n; ) {
        const unsigned char *e = memchr(p + off, 'l', (size_t)(n - off));
        if (!e) break;
        if (memcmp(e, "logfile=", 8) == 0 &&
            (e == p || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) {
            const unsigned char *s = e + 8;
            size_t v;
            const unsigned char *lineend = memchr(s, '\n', (size_t)(n - (size_t)(s - buf)));
            v = lineend ? (size_t)(lineend - s) : (size_t)(n - (size_t)(s - buf));
            while (v > 0 && (s[v - 1] == '\r' || s[v - 1] == ' ' || s[v - 1] == '\t')) v--;
            while (v > 0 && (*s == ' ' || *s == '\t')) { s++; v--; }
            if (v > 0 && *s == '"' && s[v - 1] == '"' && v > 1) { s++; v -= 2; }
            if (v > 0 && v < sizeof(val)) {
                memcpy(val, s, v); val[v] = 0;
                if (val[0] == '/')
                    snprintf(path, sizeof(path), "%s", val);
                else
                    snprintf(path, sizeof(path), "%s/%s.log", LK_LOG_DIR, val);
                break;
            }
        }
        off = (unsigned)(e - p) + 1;
    }
    if (!path[0]) return;
    if (lk_fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        /* 已存在：属主=job UID 不动；属主=保留段（槽位复用）或 root →
           chown 给 job；其余属主不碰（app 写不进也只是写不进）。 */
        if (!S_ISREG(st.st_mode) || st.st_uid == j->uid) return;
        if (st.st_uid != 0 && (st.st_uid < UID_BASE || st.st_uid >= UID_BASE + LK_MAX_SLOTS)) return;
        fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return;
        if (fchown(fd, j->uid, j->uid) != 0) { close(fd); return; }
        close(fd);
        return;
    }
    if (errno != ENOENT) return;
    fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (fchown(fd, j->uid, j->uid) != 0) { close(fd); return; }
    close(fd);
}
static int begin_start(struct lk_request *q,int c) {
    int i,home=-1,output[2]={-1,-1},ex[2]={-1,-1},e;
    uint64_t total,available; unsigned count; struct job *j=NULL; struct procinfo p;
    pid_t keeper=getpid();
    if (safe_mode||persist_broken||quitting||!lx06_scripts_allowed()) return EPERM;
    if (meminfo(&total,&available)) { latch_safe("meminfo-unreadable"); return EIO; }
    if (!budget||available<reserve_kb) { latch_safe("low-available-memory"); return ENOMEM; }
    unsigned active=0;
    for(i=0;i<LK_MAX_SLOTS;i++)if(jobs[i].used)active++;
    if(active>=LK_MAX_JOBS)return EUSERS;
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&!strcmp(jobs[i].name,q->name)) return EBUSY;
    for (i=0;i<LK_MAX_SLOTS;++i) if (!strcmp(slots[i],q->name)) { j=&jobs[i]; break; }
    if (!j) for (i=0;i<LK_MAX_SLOTS;++i) if (!slots[i][0]) { j=&jobs[i]; break; }
    if (!j) return EUSERS;
    /* LK_ROOT 保持 root：与保留 UID 段无关，跳过冲突/残留检查。 */
    if (!(q->flags&LK_ROOT)) {
        if (raw_uid_exists((uid_t)(UID_BASE+i))<0) return EADDRINUSE;
        if (raw_gid_exists((gid_t)(UID_BASE+i))<0) return EADDRINUSE;
        if (scan(0,UID_BASE+i,&count,NULL)) { latch_safe("proc-scan-failed"); return EIO; }
        if (count) { latch_safe("start-reserved-uid-residual"); return EBUSY; }
    }
    if ((q->flags&LK_AUTO)&&auto_latch()) {
        e=errno; if (e!=EALREADY) latch_safe("auto-latch-read-or-write-failed"); return e;
    }
    if (!slots[i][0]) {
        struct stat st;
        if (!lk_fstatat(userfd,q->name,&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT) {
            latch_safe("unregistered-workspace: manual repair required"); return EPERM;
        }
        strcpy(slots[i],q->name); slot_root[i]=(q->flags&LK_ROOT)?1:0;
        if (save_slots()) return EIO;
    }
    memset(j,0,sizeof(*j)); j->uid=(q->flags&LK_ROOT)?0:(uid_t)(UID_BASE+i);
    j->pipefd=j->execfd=j->logfd=-1; j->client=-1;
    snprintf(j->name,sizeof(j->name),"%s",q->name);
    home=prepare_home(j); if (home<0) return errno;
    /* runlog 链接预创建（best-effort）：非 root job 不能在 root 0755 目录
       建文件，脚本要写的链接日志须先由 keeper 建好并 chown 到 job UID。 */
    ensure_loglink(q, j);
    if (!(q->flags&LK_LOG_OFF)) {
        char leaf[96]; struct stat st;
        snprintf(leaf,sizeof(leaf),"%s.log",j->name);
        j->logfd=openat(logdirfd,leaf,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
        if (j->logfd<0||lk_fstat(j->logfd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||
            st.st_nlink!=1||(st.st_mode&0022)||ftruncate(j->logfd,0)) goto fail;
    }
    if (pipe2(output,O_CLOEXEC)||pipe2(ex,O_CLOEXEC|O_NONBLOCK)||fcntl(output[0],F_SETFL,O_NONBLOCK)) goto fail;
    /* fork 前落盘 ACTIVE；掉电/被 kill 后不自动重跑。 */
    active_state=1; if (persist_state()) goto fail;
    j->used=1; j->client=c; j->exec_until=lk_now_ms()+250;
    j->until=q->timeout_sec?lk_now_ms()+(int64_t)q->timeout_sec*1000:0;
    j->pid=fork();
    if (j->pid<0) { j->used=0; goto fail; }
    if (!j->pid) child_exec(q,j,home,output[1],ex[1],keeper);
    close(home); close(output[1]); close(ex[1]); j->pipefd=output[0]; j->execfd=ex[0];
    if (read_proc(j->pid,&p)!=1) { latch_safe("child-identity-unreadable"); return EIO; }
    j->ticks=p.ticks; clients[c].pending=1; return 0;
fail:
    e=errno?errno:EIO;
    if (home>=0) close(home);
    if (output[0]>=0) close(output[0]);
    if (output[1]>=0) close(output[1]);
    if (ex[0]>=0) close(ex[0]);
    if (ex[1]>=0) close(ex[1]);
    if (j->logfd>=0) close(j->logfd);
    j->logfd=-1;
    if (safe_mode) latch_safe("start-state-failure");
    return e;
}
/* ---------- root 执行（LK_ROOT）扩展进程跟踪 ----------
 * root 执行 job 的 uid=0，reserved UID 巡检（scan）对它整体失效；其 daemon
 * 扩展进程同样被本 daemon 以 subreaper 收养（ppid=keeper），故按 PID 跟踪。
 * ext[]    = 存活 root 扩展进程表（~100ms 刷新；预算/拉闸/完成判定用）。
 * stop_pre = root job 停止时刻的既有孤儿快照（停止只杀“停止后新出现的孤儿”
 *            = 本 job 的扩展进程，不误杀其他 root 应用的扩展）。 */
#define LK_EXT_MAX 32
struct lk_ext { pid_t pid; uint64_t ticks; int slot; };
static struct lk_ext ext[LK_EXT_MAX]; static int ext_n=0;
static pid_t stop_pre[LK_EXT_MAX]; static int stop_pre_n=0; static int64_t stop_pre_at=0;
/* 列出 ppid=本 daemon 的进程（排除 excl 列表，如各 job launcher）。0/-1。 */
static int list_ppid_procs(const pid_t *excl,int excl_n,pid_t *out,int max,int *out_n) {
    int fd,n,err=0,i; char *end; long id; struct procinfo p;
    char db[2048] __attribute__((aligned(8)));
    pid_t kp=getpid();
    *out_n=0;
    if (max>LK_EXT_MAX) max=LK_EXT_MAX;
    fd=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (;;) {
        n=lk_getdents(fd,db,sizeof db);
        if (n<0) { err=1; break; }
        if (!n) break;
        for (size_t o=0;o<(size_t)n;) {
            struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
            o+=(size_t)de->d_reclen;
            if (de->d_name[0]<'0'||de->d_name[0]>'9') continue;
            id=strtol(de->d_name,&end,10);
            if (*end||id<=0||id>INT_MAX) continue;
            { int r=read_proc((pid_t)id,&p);
              if (r<=0) continue;
              if (p.ppid!=kp) continue;
              for (i=0;i<excl_n;++i) if (excl[i]==p.pid) break;
              if (i<excl_n) continue;
              if (out) {
                  if (*out_n>=max) { err=1; continue; }
                  out[(*out_n)++]=p.pid;
              } else ++*out_n;
            }
        }
    }
    close(fd);
    return err?-1:0;
}
/* 信号“stop_pre 快照之后新出现的孤儿”（= 被停 job 的扩展进程）。0/-1。 */
static int scan_new_orphans(int sig,unsigned *count) {
    int fd,n,err=0,i; char *end; long id; struct procinfo p;
    char db[2048] __attribute__((aligned(8)));
    pid_t kp=getpid();
    if (count) *count=0;
    fd=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return -1;
    for (;;) {
        n=lk_getdents(fd,db,sizeof db);
        if (n<0) { err=1; break; }
        if (!n) break;
        for (size_t o=0;o<(size_t)n;) {
            struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
            o+=(size_t)de->d_reclen;
            if (de->d_name[0]<'0'||de->d_name[0]>'9') continue;
            id=strtol(de->d_name,&end,10);
            if (*end||id<=0||id>INT_MAX) continue;
            { int r=read_proc((pid_t)id,&p);
              if (r<=0) continue;
              if (p.ppid!=kp) continue;
              /* A registered launcher is never an orphan. Its own job sends
               * identity-checked signals; stopping one root app must not kill
               * another app that shares uid=0 and this subreaper parent. */
              for (i=0;i<LK_MAX_SLOTS;++i)
                  if (jobs[i].used && jobs[i].pid==p.pid && jobs[i].ticks==p.ticks) break;
              if (i<LK_MAX_SLOTS) continue;
              for (i=0;i<stop_pre_n;++i) if (stop_pre[i]==p.pid) break;
              if (i<stop_pre_n) continue;
              if (count) ++*count;
              if (sig) identity_signal(&p,sig);
            }
        }
    }
    close(fd);
    return err?-1:0;
}
/* 刷新 ext[]：归属=最近启动的活动 root job（exec_until 单调）；无活动则
 * 归属停止中的 root job；均无则 slot=-1（不挂预算）。扫描失败保留旧表。 */
static void ext_refresh(void) {
    pid_t list[LK_EXT_MAX]; int n=0,i; int roots=0,rslot=-1;
    pid_t launchers[LK_MAX_SLOTS]; int launcher_count=0;
    int64_t newest=0;
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&jobs[i].uid==0) {
        ++roots;
        if (jobs[i].exec_until>newest) { newest=jobs[i].exec_until; rslot=i; }
    }
    if (!roots)
        for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].stopping&&jobs[i].uid==0) { rslot=i; break; }
    for (i=0;i<LK_MAX_SLOTS;++i) if(jobs[i].used) launchers[launcher_count++]=jobs[i].pid;
    if (list_ppid_procs(launchers,launcher_count,list,LK_EXT_MAX,&n)) return;
    ext_n=0;
    for (i=0;i<n&&ext_n<LK_EXT_MAX;++i) {
        struct procinfo p;
        if (read_proc(list[i],&p)!=1) continue;
        ext[ext_n].pid=list[i]; ext[ext_n].ticks=p.ticks; ext[ext_n].slot=rslot;
        ++ext_n;
    }
}
static int ext_count_for(int slot) {
    int i,c=0;
    for (i=0;i<ext_n;++i) if (ext[i].slot==slot) ++c;
    return c;
}
static int ext_find(pid_t pid) {
    int i;
    for (i=0;i<ext_n;++i) if (ext[i].pid==pid) return i;
    return -1;
}
static void stop_job(struct job *j,int64_t now) {
    if (j->stopping) return;
    j->stopping=1; j->kill_at=now+250; signal_launcher(j,SIGTERM);
    if (scan(SIGTERM,j->uid,NULL,NULL)) latch_safe("stop-proc-scan-failed");
    if (j->uid==0) {
        /* root 执行：快照停止前既有孤儿（其他 root 应用的扩展进程），
           停止过程只杀新出现的孤儿（= 本 job 的扩展）。 */
        pid_t excl[LK_MAX_SLOTS]; int ex=0,n=0,i;
        for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) excl[ex++]=jobs[i].pid;
        stop_pre_n=0;
        if (!list_ppid_procs(excl,ex,stop_pre,LK_EXT_MAX,&n)) stop_pre_n=n;
        stop_pre_at=now;
        if (scan_new_orphans(SIGTERM,NULL)) latch_safe("stop-proc-scan-failed");
    }
}
/* 只在 UID 停净后删受管 workspace；不跟随目录内链接。 */
static int remove_tree(int parent,const char *name,unsigned depth,unsigned *left,int64_t until) {
    struct stat st; int fd,ret=0;
    if (!*left||lk_now_ms()>=until) { errno=EBUSY; return -1; }
    --*left;
    if (lk_fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW)) return errno==ENOENT?0:-1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(parent,name,0);
    if (depth>32) { errno=ELOOP; return -1; }
    fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    {
        char db[1024] __attribute__((aligned(8)));
        /* 裸 getdents64 递归：fdopendir/readdir 内部 fstatat → oops。 */
        for (;;) {
            int n=lk_getdents(fd,db,sizeof db);
            if (n<0) { close(fd); return -1; }
            if (!n) break;
            for (size_t o=0;o<(size_t)n;) {
                struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
                size_t next=o+(size_t)de->d_reclen;
                if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) { o=next; continue; }
                if (remove_tree(fd,de->d_name,depth+1,left,until)) { ret=-1; break; }
                o=next;
            }
            if (ret) break;
        }
    }
    close(fd);
    if (!ret) ret=unlinkat(parent,name,AT_REMOVEDIR);
    return ret;
}
/* 卸载清扫：/data/lx06 根目录顶层中属主为该 job UID 的业务载荷条目
   （部署时应用自建的目录/文件，如 /data/lx06/<app>）一并删除。
   root/其他 UID 一概不碰（官方与系统文件安全）。有界预算 best-effort：
   预算耗尽不使卸载失败——槽位仍登记，下次 FORGET 重试续清。
   必须在 save_slots 之前调用：槽位未释放时同 UID 不可被新 job 占用，
   清扫不会误删新部署的文件。 */
static void sweep_job_uid(int slot) {
    uid_t uid=(uid_t)(UID_BASE+slot);
    char db[1024] __attribute__((aligned(8)));
    unsigned left=2048;
    int rootfd;
    int64_t until;
    rootfd=open("/data/lx06",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (rootfd<0) return;
    until=lk_now_ms()+800;
    for (;;) {
        int n=lk_getdents(rootfd,db,sizeof db);
        size_t o;
        if (n<0||!n) break;
        for (o=0;o<(size_t)n;++o) {
            struct lk_dent64 *de=(struct lk_dent64 *)(db+o);
            struct stat st;
            o+=(size_t)de->d_reclen;
            if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
            if (lk_fstatat(rootfd,de->d_name,&st,AT_SYMLINK_NOFOLLOW)) continue;
            if (st.st_uid!=uid) continue;
            (void)remove_tree(rootfd,de->d_name,0,&left,until);
        }
        if (lk_now_ms()>=until) break; /* 预算到：剩余留给重试 */
    }
    close(rootfd);
}
static int forget(const char *name) {
    int i,slot=-1; char leaf[96]; unsigned count,left=96;
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&!strcmp(jobs[i].name,name)) {
        stop_job(&jobs[i],lk_now_ms()); return EBUSY;
    }
    for (i=0;i<LK_MAX_SLOTS;++i) if (!strcmp(slots[i],name)) { slot=i; break; }
    if (slot<0) {
        struct stat st;
        /* 未运行过的上传没有 UID/workspace，FORGET 必须幂等，方便 webd 删除脚本。
         * 孤儿 workspace（旧应用残留、不在槽位注册表）：用户显式卸载，允许递归删除
         * ——即使 persist_broken 也放行（不动注册表、不猜 UID）；
         * 清理后工作区干净则自动释放 persist 锁（try_clear_persist）。 */
        if (!lk_fstatat(userfd,name,&st,AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISDIR(st.st_mode)) return EPERM;
            /* 该 UID 是某在用槽位的保留段，其进程可能持有目录内文件 → 等它停。 */
            if (st.st_uid>=UID_BASE&&st.st_uid<UID_BASE+LK_MAX_SLOTS)
                for (i=0;i<LK_MAX_SLOTS;++i)
                    if (jobs[i].used&&jobs[i].uid==st.st_uid) return EBUSY;
        } else if (errno!=ENOENT) return errno;
    } else {
        /* 注册槽位：persist_broken 时 UID 映射未知，仍禁删（防误删在跑槽位）。 */
        if (persist_broken) return EIO;
        if (slot_root[slot]) { if (ext_count_for(slot)) return EBUSY; }
        else if (scan(0,UID_BASE+slot,&count,NULL)||count) return EBUSY;
    }
    /* 有界分批删除，EBUSY 表示尚未删净，调用者可以显式重试 FORGET。
     * 单个不可中断的内核文件系统调用仍不属于用户态硬实时保证。 */
    if (remove_tree(userfd,name,0,&left,lk_now_ms()+40)) return errno;
    snprintf(leaf,sizeof(leaf),"%s.log",name);
    if (unlinkat(logdirfd,leaf,0)&&errno!=ENOENT) return errno;
    snprintf(leaf,sizeof(leaf),"%s.log.1",name);
    if (unlinkat(logdirfd,leaf,0)&&errno!=ENOENT) return errno;
    {
        const char *suffix[]={"rc","pid","st"}; unsigned k;
        for (k=0;k<sizeof(suffix)/sizeof(suffix[0]);++k) {
            snprintf(leaf,sizeof(leaf),".%s.%s",name,suffix[k]);
            if (unlinkat(uploadfd,leaf,0)&&errno!=ENOENT) return errno;
        }
    }
    if (fsync(userfd)||fsync(logdirfd)||fsync(uploadfd)) {
        latch_safe("forget-directory-sync-failed"); return EIO;
    }
    if (slot>=0) {
        /* 槽位仍持有期间清扫该 UID 的业务载荷（/data/lx06/<app> 等），
           卸载不留业务文件；best-effort，失败不阻塞槽位释放。
           root 槽无保留 UID 段，跳过清扫（业务目录由部署脚本自管）。 */
        if (!slot_root[slot]) sweep_job_uid(slot);
        slots[slot][0]=0; slot_root[slot]=0;
        if (save_slots()) return EIO;
    }
    /* 上传脚本/持久脚本/.bk/appmeta 由 webd 删除，避免跨管理者竞态。 */
    (void)try_clear_persist(); /* 孤儿清理后工作区干净 → 自动释放 persist 锁 */
    return 0;
}
static int validate(struct lk_request *q) {
    unsigned i;
    if (q->magic!=LK_MAGIC||q->reserved||q->action<LK_START||q->action>LK_FORGET||
        !memchr(q->name,0,sizeof(q->name))) return EPROTO;
    for (i=0;i<LK_ARGC;++i) if (!memchr(q->argv[i],0,LK_ARG_LEN)) return EINVAL;
    if (q->action!=LK_START) {
        if (q->flags||q->argc||q->timeout_sec) return EINVAL;
        if ((q->action==LK_STATUS||q->action==LK_RECOVER)&&q->name[0]) return EINVAL;
        if ((q->action==LK_FORGET||(q->action==LK_STOP&&q->name[0]))&&!valid_name(q->name)) return EINVAL;
        return 0;
    }
    if (!valid_name(q->name)||!q->argc||q->argc>LK_ARGC||
        (q->flags&~(LK_AUTO|LK_WIDE_VM|LK_LOG_OFF|LK_ROOT))||q->timeout_sec>86400) return EINVAL;
    for (i=0;i<q->argc;++i) if (!memchr(q->argv[i],0,LK_ARG_LEN)) return EINVAL;
    return q->argv[0][0]!='/'||!q->argv[0][1]?EINVAL:0;
}
static void request(int c) {
    struct lk_request q;
    ssize_t n=recv(clients[c].fd,&q,sizeof(q),MSG_TRUNC|MSG_DONTWAIT);
    int e,i,found=0; unsigned count;
    if (n<0&&(errno==EAGAIN||errno==EINTR)) return;
    if (n!=(ssize_t)sizeof(q)) { answer(c,EPROTO,NULL); return; }
    e=validate(&q); if (e) { answer(c,e,NULL); return; }
    switch (q.action) {
    case LK_START:
        e=begin_start(&q,c); if (e) answer(c,e,NULL); return;
    case LK_STOP:
        for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&(!q.name[0]||!strcmp(q.name,jobs[i].name))) {
            stop_job(&jobs[i],lk_now_ms()); found=1;
        }
        if (q.name[0]&&!found) {
            /* 幂等：job 已收尸清表。槽位已注册且该身份无残留进程 → 视为已停净
               （UI 对已退出应用点停止不应报「未确认停净」）；仍有残留 → EBUSY
               由调用者有界重试；从未注册的名字保持 ENOENT。 */
            int slot=-1;
            for (i=0;i<LK_MAX_SLOTS;++i) if (!strcmp(slots[i],q.name)) { slot=i; break; }
            if (slot<0) e=ENOENT;
            else if (slot_root[slot]) e=ext_count_for(slot)?EBUSY:0;
            else if (scan(0,UID_BASE+slot,&count,NULL)) e=EIO;
            else e=count?EBUSY:0;
        } else e=0;
        break;
    case LK_STATUS: break;
    case LK_RECOVER:
        if (persist_broken) {
            /* 重新校验：孤儿目录已清理（UI 卸载 / SSH 删除）→ 释放锁继续正常恢复。 */
            if (workspace_dirty()) { e=EIO; break; }
            persist_broken=0;
        }
        /* 重读槽位表：启动时判 corrupt 会让 slots[] 清空且不再重载，
           磁盘上已注册的同名 home 目录随后被 begin_start 误判
           “unregistered-workspace”，daemon 永不恢复（只能重启进程）。
           这里重读：文件有效且工作区干净 → slots 复原；仍 corrupt →
           persist_broken 重新置位，下方 EIO 上报人工处理。 */
        load_slots();
        if (persist_broken) { e=EIO; break; }
        if (scan(0,0,&count,NULL)||count) { e=EBUSY; break; }
        for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) { e=EBUSY; break; }
        if (!e) {
            uint64_t t,a;
            if (meminfo(&t,&a)||a<reserve_kb||!budget) { e=ENOMEM; break; }
            safe_mode=0; active_state=0; reason[0]=0;
            if (persist_state()) { latch_safe("recover-write-failed"); e=EIO; }
        }
        break;
    case LK_FORGET: e=forget(q.name); break;
    }
    answer(c,e,NULL);
}
static void accept_clients(void) {
    int n;
    /* 每轮有界，连接洪泛不能无限占用主循环。 */
    for (n=0;n<NCLIENT;++n) {
        struct ucred u; socklen_t z=sizeof(u);
        int fd=accept4(listenfd,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC),i;
        if (fd<0) break;
        if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&u,&z)||u.uid!=0) { close(fd); continue; }
        for (i=0;i<NCLIENT&&clients[i].fd>=0;++i) {}
        if (i==NCLIENT) { close(fd); continue; }
        clients[i].fd=fd; clients[i].pending=0; clients[i].until=lk_now_ms()+350;
    }
}
static void drain(struct job *j) {
    char b[4096]; ssize_t n;
    if (j->pipefd<0) return;
    /* 每 job 每轮最多 4KB；stdout/stderr 共一个 64KB tmpfs 日志。 */
    n=read(j->pipefd,b,sizeof(b));
    if (!n) { close(j->pipefd); j->pipefd=-1; }
    if (n>0&&j->logfd>=0) {
        char name[LK_NAME_LEN + 5];
        snprintf(name, sizeof name, "%s.log", j->name);
        if (lx06_log_append_fd(logdirfd, name, j->logfd, b, (size_t)n)) {
            close(j->logfd);
            j->logfd=-1;
        }
    }
}
static void exec_progress(struct job *j,int64_t now) {
    int err=0,round; ssize_t n=-1;
    if (j->execfd<0) return;
    for (round=0;round<3;++round) {
        n=read(j->execfd,&err,sizeof(err));
        if (n<0&&(errno==EAGAIN||errno==EINTR)&&now<j->exec_until) return;
        if (n<0) { err=now>=j->exec_until?ETIMEDOUT:errno; break; }
        if (!n) {
            siginfo_t si;
            if (!j->exec_ready) { err=ECHILD; latch_safe("exec-handshake-incomplete"); }
            else {
                memset(&si,0,sizeof(si));
                if (waitid(P_PID,(id_t)j->pid,&si,WEXITED|WNOHANG|WNOWAIT)) err=errno;
                else if (si.si_pid&&(si.si_code==CLD_KILLED||si.si_code==CLD_DUMPED)) {
                    err=ECHILD; latch_safe("child-killed-during-exec-handshake");
                }
            }
            break;
        }
        if (n!=(ssize_t)sizeof(err)||err<0) { err=EPROTO; break; }
        if (err) break;
        if (j->exec_ready) { err=EPROTO; break; }
        j->exec_ready=1;
    }
    if (round==3) err=EPROTO;
    close(j->execfd); j->execfd=-1;
    if (!err) {
        char b[80]; snprintf(b,sizeof(b),"%ld %llu\n",(long)j->pid,(unsigned long long)j->ticks);
        if (safe_mode||j->stopping||!lx06_scripts_allowed()) err=ECANCELED;
        else if (metadata(j->name,"pid",b)||metadata(j->name,"st","running\n")) {
            err=EIO; latch_safe("start-metadata-write-failed");
        }
    }
    if (err) { stop_job(j,now); if (err==ETIMEDOUT) latch_safe("exec-handshake-timeout"); }
    if (j->client>=0&&clients[j->client].fd>=0) answer(j->client,err,err?NULL:j);
    j->client=-1;
}
/* 良性退出判定：exit 0 = 正常；被 SIGTERM/SIGKILL 结束 = "已停止"
   （webd 的 timeout 杀卸载脚本、会话超时 kill 子进程、keeper 自身升级杀
   都属于预期路径，不能触发安全模式）；其余信号（SEGV/ABRT/FPE/...）
   仍是崩溃。 */
static int exit_benign(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status)==0;
    if (WIFSIGNALED(status)) {
        int s=WTERMSIG(status);
        return s==SIGTERM||s==SIGKILL;
    }
    return 0;
}
static void tick_jobs(int64_t now) {
    /* Disabling user code is an administrative stop, never a safe-mode fault. */
    static int64_t next_policy_check;
    if(now>=next_policy_check) {
        next_policy_check=now+1000;
        if(!lx06_scripts_allowed())
            for(int k=0;k<LK_MAX_SLOTS;k++)if(jobs[k].used&&!jobs[k].stopping)stop_job(&jobs[k],now);
    }
    int i,status,work; pid_t pid;
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) { drain(&jobs[i]); exec_progress(&jobs[i],now); }
    /* 收割也要有界；持续短进程不能饿死 watchdog/RPC。waitid 的 real UID
     * 在 waitpid 回收前取出，可识别 subreaper 收养的 setsid 后代异常。 */
    for (work=0;work<LK_MAX_SLOTS*16;++work) {
        siginfo_t si;
        memset(&si,0,sizeof(si));
        if (waitid(P_ALL,0,&si,WEXITED|WNOHANG|WNOWAIT)) {
            if (errno!=ECHILD&&errno!=EINTR) latch_safe("waitid-failed");
            break;
        }
        if (!si.si_pid) break;
        pid=waitpid(si.si_pid,&status,WNOHANG);
        if (pid<=0) break;
        for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used&&!jobs[i].reaped&&jobs[i].pid==pid) {
            struct job *j=&jobs[i]; j->reaped=1; j->status=status;
            if (!j->stopping&&!exit_benign(status))
                latch_safe("job-abnormal-exit: all extensions quarantined");
            /* rc=0 的 daemonizing leader 不是 STOP：后台进程仍按 UID 监管。 */
            break;
        }
        if (i==LK_MAX_SLOTS) {
            int xi=ext_find(si.si_pid);
            if (si.si_uid>=UID_BASE&&si.si_uid<UID_BASE+LK_MAX_SLOTS) {
                struct job *j=&jobs[si.si_uid-UID_BASE];
                if (!j->stopping&&!exit_benign(status))
                    latch_safe("descendant-abnormal-exit: all extensions quarantined");
            } else if (xi>=0&&ext[xi].slot>=0&&jobs[ext[xi].slot].used) {
                struct job *j=&jobs[ext[xi].slot];
                if (!j->stopping&&!exit_benign(status))
                    latch_safe("descendant-abnormal-exit: all extensions quarantined");
                ext[xi]=ext[ext_n-1]; --ext_n; /* 离表，完成判定立即可见 */
            }
        }
    }
    for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) {
        struct job *j=&jobs[i]; unsigned count;
        if (j->until&&now>=j->until&&!j->stopping) latch_safe("job-timeout");
        if (j->reaped||(j->stopping&&now>=j->kill_at)) {
            if (j->uid==0) {
                /* root 执行 job：uid=0 全系统共享，UID 巡检失效，按 pid 树判定。
                   自然完成 = launcher 已收尸 && 无跟踪扩展进程；
                   STOP = launcher 收尸 + 无“停止后新孤儿”，5s 预算。 */
                unsigned oc=0;
                ext_refresh(); /* 完成判定前即时刷新，避免 100ms 表龄误判 */
                if (j->stopping) {
                    if (now>=j->kill_at) signal_launcher(j,SIGKILL);
                    if (scan_new_orphans(now>=j->kill_at?SIGKILL:SIGTERM,&oc))
                        latch_safe("kill-proc-scan-failed");
                    if (!oc||now-stop_pre_at>5000) {
                        int rc=WIFEXITED(j->status)?WEXITSTATUS(j->status):128+WTERMSIG(j->status);
                        result_files(j,rc);
                        if (j->pipefd>=0) close(j->pipefd);
                        if (j->execfd>=0) close(j->execfd);
                        if (j->logfd>=0) close(j->logfd);
                        j->used=0; stop_pre_n=0;
                    }
                } else if (j->reaped&&ext_count_for(i)==0) {
                    int rc=WIFEXITED(j->status)?WEXITSTATUS(j->status):128+WTERMSIG(j->status);
                    result_files(j,rc);
                    if (j->pipefd>=0) close(j->pipefd);
                    if (j->execfd>=0) close(j->execfd);
                    if (j->logfd>=0) close(j->logfd);
                    j->used=0;
                }
                continue;
            }
            if (j->stopping&&now>=j->kill_at) signal_launcher(j,SIGKILL);
            if (j->stopping&&now>=j->kill_at&&scan(SIGKILL,j->uid,NULL,NULL)) latch_safe("kill-proc-scan-failed");
            if (j->reaped&&!scan(0,j->uid,&count,NULL)&&!count) {
                int rc=WIFEXITED(j->status)?WEXITSTATUS(j->status):128+WTERMSIG(j->status);
                result_files(j,rc);
                if (j->pipefd>=0) close(j->pipefd);
                if (j->execfd>=0) close(j->execfd);
                if (j->logfd>=0) close(j->logfd);
                j->used=0;
            }
        }
    }
}
static void watch_memory(void) {
    uint64_t t,a,rss; unsigned count; int i;
    if (scan(0,0,&count,&rss)) { latch_safe("proc-scan-failed"); return; }
    /* root 执行 job 的扩展进程（uid=0）并入系统预算与 job 成员计数。 */
    ext_refresh();
    for (i=0;i<ext_n;++i) {
        struct procinfo p;
        if (read_proc(ext[i].pid,&p)!=1) continue;
        ++count; rss+=p.rss;
        if (ext[i].slot>=0&&jobs[ext[i].slot].used) {
            ++jobs[ext[i].slot].members; jobs[ext[i].slot].rss+=p.rss;
        }
    }
    for (i=0;i<LK_MAX_SLOTS;++i) if (!jobs[i].used&&jobs[i].members) {
        latch_safe("unknown-reserved-uid-process"); break;
    }
    if (meminfo(&t,&a)) latch_safe("meminfo-unreadable");
    else if (rss>budget||(count&&a<reserve_kb)) latch_safe("extension-rss-or-memory-pressure");
    if (safe_mode&&count) (void)scan(SIGKILL,0,NULL,NULL);
    if (safe_mode) for (i=0;i<ext_n;++i) {
        struct procinfo p;
        if (read_proc(ext[i].pid,&p)==1) (void)kill(ext[i].pid,SIGKILL);
    }
    if (!count&&active_state) {
        for (i=0;i<LK_MAX_SLOTS&&!jobs[i].used;++i) {}
        if (i==LK_MAX_SLOTS) { active_state=0; if (persist_state()) latch_safe("active-clear-write-failed"); }
    }
}
static void on_signal(int sig) { (void)sig; quitting=1; }
static int daemon_main(void) {
    struct sockaddr_un sa; struct stat st; uint64_t t,a;
    int i; int64_t next_watch,shutdown_at=0;
    sigset_t empty;
    if (geteuid()!=0) { errno=EPERM; return -1; }
    /* init/调用者可能忽略 SIGCHLD，继承会丢失真实 wait status。 */
    sigemptyset(&empty);
    if (signal(SIGCHLD,SIG_DFL)==SIG_ERR||sigprocmask(SIG_SETMASK,&empty,NULL)) return -1;
    umask(022);
    for (i=0;i<NCLIENT;++i) clients[i].fd=-1;
    for (i=0;i<LK_MAX_SLOTS;++i) jobs[i].pipefd=jobs[i].execfd=jobs[i].logfd=-1;
    statefd=open_dir(LK_STATE_DIR,0700,1); runfd=open_dir(LK_RUN_DIR,0700,1);
    userfd=open_dir(LK_USER_DIR,0755,1); logdirfd=open_dir(LK_LOG_DIR,0755,1);
    uploadfd=open_dir(LK_UPLOAD_DIR,0700,1);
    if (statefd<0||runfd<0||userfd<0||logdirfd<0||uploadfd<0) return -1;
    lockfd=openat(runfd,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lockfd<0||trusted(lockfd,0600,1)||flock(lockfd,LOCK_EX|LOCK_NB)) return -1;
    /* 先锁再 unlink，第二 daemon 不得破坏在线 socket。 */
    if (strlen(LK_SOCKET)>=sizeof(sa.sun_path)) { errno=ENAMETOOLONG; return -1; }
    if (lk_lstat(LK_SOCKET,&st)) {
        if (errno!=ENOENT) return -1;
    } else {
        if (!S_ISSOCK(st.st_mode)||st.st_uid!=0) { errno=EPERM; return -1; }
        if (unlink(LK_SOCKET)) return -1;
    }
    /* 裸 open/read：fopen → 64 位 fstat → oops。boot_id 恰为 36 字节 + '\n'。
       必须在 load_state 之前读：LK2 的 stale-active 判定依赖当前 boot_id。 */
    { int bfd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
      if (bfd>=0) {
          ssize_t bn=read(bfd,boot_id,sizeof(boot_id)-1);
          close(bfd);
          if (bn!=37||boot_id[bn-1]!='\n') boot_id[0]=0; else boot_id[bn]=0;
      }
    }
    load_state(); load_slots();
    if (meminfo(&t,&a)) { budget=0; reserve_kb=16384; latch_safe("startup-meminfo-unreadable"); }
    else { budget=lk_budget_kb(t,a); reserve_kb=t/16;
        if (reserve_kb<8192) reserve_kb=8192;
        if (!budget) latch_safe("startup-insufficient-memory");
    }
    /* 收养双 fork 后代便于回收僵尸；主隔离仍按 reserved UID，不按进程组。 */
    if (prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)) latch_safe("subreaper-unavailable");
    watch_memory();
    listenfd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (listenfd<0) return -1;
    memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX; strcpy(sa.sun_path,LK_SOCKET);
    if (bind(listenfd,(struct sockaddr *)&sa,sizeof(sa))||chmod(LK_SOCKET,0600)||listen(listenfd,16)) return -1;
    signal(SIGPIPE,SIG_IGN); signal(SIGTERM,on_signal); signal(SIGINT,on_signal);
    next_watch=lk_now_ms()+1000;
    for (;;) {
        struct pollfd p={listenfd,POLLIN,0}; int64_t now=lk_now_ms();
        if (now<0) { latch_safe("monotonic-clock-failed"); break; }
        if (quitting&&!shutdown_at) {
            shutdown_at=now+2000;
            for (i=0;i<LK_MAX_SLOTS;++i) if (jobs[i].used) stop_job(&jobs[i],now);
        }
        if (!quitting) accept_clients();
        for (i=0;i<NCLIENT;++i) if (clients[i].fd>=0) {
            if (now>=clients[i].until) { close_client(i); continue; }
            if (!clients[i].pending) request(i);
        }
        tick_jobs(now);
        if (now>=next_watch||quitting) { watch_memory(); next_watch=now+1000; }
        if (quitting&&(!active_state||now>=shutdown_at)) break;
        (void)poll(&p,1,20);
    }
    if (active_state) latch_safe("daemon-exit-with-residuals");
    unlink(LK_SOCKET); return 0;
}
static int copy_arg(char *dst,size_t n,const char *s) {
    if (strlen(s)>=n) return -1;
    strcpy(dst,s); return 0;
}
static void print_json(const struct lk_reply *r) {
    unsigned i;
    printf("{\"safe_mode\":%u,\"reason\":\"%s\",\"pid\":%lld,\"start_ticks\":%llu,"
           "\"rss_kb\":%llu,\"budget_kb\":%llu,\"jobs\":%u,\"job\":[",
           r->safe_mode,r->reason,(long long)r->pid,(unsigned long long)r->start_ticks,
           (unsigned long long)r->rss_kb,(unsigned long long)r->budget_kb,r->jobs);
    for (i=0;i<r->jobs;++i) printf("%s{\"name\":\"%s\",\"pid\":%lld,\"start_ticks\":%llu,\"uid\":%u,\"rss_kb\":%llu,\"members\":%u}",
        i?",":"",r->job[i].name,(long long)r->job[i].pid,(unsigned long long)r->job[i].start_ticks,
        r->job[i].uid,(unsigned long long)r->job[i].rss_kb,r->job[i].members);
    puts("]}");
}
#ifndef LK_NO_MAIN
/* Fixed destinations are root-owned. Drain the input even when a log cannot
 * be written so a diagnostic never causes the producing service to fail. */
static int log_stream(const char *key) {
    static const struct {
        const char *key, *directory, *file;
        mode_t mode;
    } logs[] = {
        {"ssh", "/tmp/lx06-logs", "ssh-auth.log", 0755},
        {"webd", "/tmp/lx06-logs", "webd.log", 0755},
        {"portwatch", "/tmp/lx06-logs", "portwatch.log", 0755},
        {"recovery", "/tmp/lx06-logs", "xiaoai-recovery.log", 0755},
        {"syslog", "/tmp/log", "messages", 0755},
        {"stat-points", "/tmp", "stat_points_rom.log", 0777},
#if defined(LK_TESTING) && defined(LK_TEST_ANCHOR)
        {"wake", LK_TEST_ANCHOR "/wake-logs", "lx06-wake.log", 0755},
#else
        {"wake", "/tmp", "lx06-wake.log", 0777},
#endif
        {"crash", "/data/lx06/logs", "last-crash.txt", 0755},
    };
    char buffer[4096];
    int dir, failed = 0;
    size_t i;
    if (geteuid() != 0) return 2;
    (void)prctl(PR_SET_NAME, "lx06-log", 0, 0, 0);
    for (i = 0; i < sizeof logs / sizeof logs[0]; ++i)
        if (!strcmp(key, logs[i].key)) break;
    if (i == sizeof logs / sizeof logs[0]) return 2;
    dir = open_dir(logs[i].directory, logs[i].mode, 1);
    if (dir < 0) failed = 1;
    for (;;) {
        ssize_t len = read(STDIN_FILENO, buffer, sizeof buffer);
        if (len < 0 && errno == EINTR) continue;
        if (len < 0) failed = 1;
        if (len <= 0) break;
        if (!failed && lx06_log_append(dir, logs[i].file, buffer, (size_t)len)) failed = 1;
    }
    if (dir >= 0) close(dir);
    return failed;
}

/* The recorder keeps an O_RDWR descriptor for its private diagnostic FIFO.
 * Make it nonblocking before capture starts. A full pipe or failed logging
 * child can then drop diagnostics without stalling capture or raising SIGPIPE. */
static int log_prepare_fifo(void) {
    struct stat st;
    int flags;
    if (geteuid() != 0 || lk_fstat(STDOUT_FILENO, &st) ||
        !S_ISFIFO(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0077)) return 2;
    flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDWR) return 2;
    return fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) < 0;
}

int main(int argc,char **argv) {
    struct lk_request q; struct lk_reply r; int i;
    if (argc == 3 && !strcmp(argv[1], "log")) {
        if (!strcmp(argv[2], "prepare-fifo")) return log_prepare_fifo();
        return log_stream(argv[2]);
    }
    memset(&q,0,sizeof(q)); q.magic=LK_MAGIC;
    if (argc==2&&!strcmp(argv[1],"daemon")) {
        if (daemon_main()) { perror("keeper daemon"); return 1; } return 0;
    }
    if (argc==2&&!strcmp(argv[1],"status")) q.action=LK_STATUS;
    else if (argc==2&&!strcmp(argv[1],"recover")) q.action=LK_RECOVER;
    else if ((argc==2||argc==3)&&!strcmp(argv[1],"stop")) {
        q.action=LK_STOP; if (argc==3&&copy_arg(q.name,sizeof(q.name),argv[2])) return 2;
    } else if (argc==3&&!strcmp(argv[1],"forget")) {
        q.action=LK_FORGET; if (copy_arg(q.name,sizeof(q.name),argv[2])) return 2;
    } else if (argc>=5&&!strcmp(argv[1],"start")) {
        q.action=LK_START; if (copy_arg(q.name,sizeof(q.name),argv[2])) return 2;
        for (i=3;i<argc&&strcmp(argv[i],"--");++i) {
            if (!strcmp(argv[i],"--auto")) q.flags|=LK_AUTO;
            else if (!strcmp(argv[i],"--wide-vm")) q.flags|=LK_WIDE_VM;
            else if (!strcmp(argv[i],"--log-off")) q.flags|=LK_LOG_OFF;
            else if (!strcmp(argv[i],"--root")) q.flags|=LK_ROOT;
            else if (!strcmp(argv[i],"--timeout")&&i+1<argc) {
                char *end; unsigned long v; errno=0; v=strtoul(argv[++i],&end,10);
                if (errno||!argv[i][0]||*end||v>86400) return 2;
                q.timeout_sec=(uint32_t)v;
            } else return 2;
        }
        if (i==argc) return 2;
        for (++i;i<argc;++i) {
            if (q.argc==LK_ARGC||copy_arg(q.argv[q.argc],LK_ARG_LEN,argv[i])) return 2;
            ++q.argc;
        }
    } else {
        fprintf(stderr,"usage: lx06-keeper daemon|status|recover|stop [NAME]|forget NAME|start NAME [--auto] [--wide-vm] [--root] [--timeout SEC] [--log-off] -- /absolute/CMD [args]\n"); return 2;
    }
    if (validate(&q)) { fprintf(stderr,"keeper: invalid request\n"); return 2; }
    if (lk_call(&q,&r,500)) { perror("keeper"); return 1; }
    print_json(&r); return 0;
}
#endif
