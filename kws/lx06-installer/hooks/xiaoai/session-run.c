/* One foreground script session and its complete player/network process group.
 * SIGUSR1 means an official wake: TERM first, bounded KILL fallback, then release
 * ownership only after descendants have exited. No engine restart or KWS stop.
 */
#define _GNU_SOURCE
#include "session-owner.h"
#include "session-epoch.h"
#include "glibc_ni_trap.h"
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifdef LX06_SESSION_PHONE_RUNTIME
#include "../../runtime/dialogue-core/mode_gate.h"
#endif

static volatile sig_atomic_t stopping;
static void stop(int signal_number) { stopping=signal_number; }
#ifndef LX06_MEMINFO_FILE
#define LX06_MEMINFO_FILE "/proc/meminfo"
#endif
static long available_kb(long *free_kb) {
    *free_kb=-1;
    char data[4096];
    int fd=open(LX06_MEMINFO_FILE,O_RDONLY|O_CLOEXEC);
    if(fd<0) return -1;
    ssize_t n=read(fd,data,sizeof(data)-1);close(fd);
    if(n<=0) return -1;
    data[n]=0;
    char *f=strstr(data,"MemFree:");
    if(f) {
        char *end;long value=strtol(f+8,&end,10);
        if(end!=f+8 && value>=0) *free_kb=value;
    }
    char *p=strstr(data,"MemAvailable:");
    if(!p) return -1;
    char *end;
    long value=strtol(p+13,&end,10);
    return end==p+13 || value<0 ? -1 : value;
}
static long long tick_ms(void) {
    struct { long sec,nsec; } ts;
    if(syscall(SYS_clock_gettime,1,&ts)) return 0;
    return (long long)ts.sec*1000+ts.nsec/1000000;
}

static int local_ticket(const char *hex,const char *expiry,unsigned char epoch[LX06_IDLE_EPOCH_BYTES],uint64_t *expires) {
    if(lx06_idle_epoch_parse(hex,epoch) || !*expiry || strlen(expiry)>17 || strspn(expiry,"0123456789")!=strlen(expiry))
        return -1;
    *expires=strtoull(expiry,NULL,10);
    return 0;
}
static int local_ticket_live(uint64_t expires) {
    uint64_t now=lx06_idle_epoch_now_ms();
    return now>0 && expires>now && expires-now<=3000;
}
#include "dialog-light-owner.h"
#include "session-resource-proof-issuer.inc"
#ifdef LX06_SESSION_PHONE_RUNTIME
/* One private request for the existing normal core. The native receiver still
 * owns saved-source selection, settings, profile matching and audio authority. */
struct phone_sender { int active,fd[2];size_t size;char request[DC_MODE_REQUEST_CAP]; };
static int phone_prepare(struct phone_sender *p,const char *name,int argc,char **argv) {
    memset(p,0,sizeof(*p));p->fd[0]=p->fd[1]=-1;
    if(strcmp(argv[0],"--phone-session"))return 0;
    if(argc!=3||getuid()!=0||geteuid()!=0||
       (strcmp(argv[1],"official_asr")&&strcmp(argv[1],"local_wake"))||
       !argv[2][0]||strlen(argv[2])>96)return -1;
    char script[129],wake[193];dc_mode_request checked;
    if(dc_mode_escape_text(name,script,sizeof script)||
       dc_mode_escape_text(argv[2],wake,sizeof wake))return -1;
    int n=snprintf(p->request,sizeof p->request,
        "{\"v\":1,\"script_id\":\"%s\",\"wake_source\":\"%s\",\"wake_text\":\"%s\",\"action\":\"call.start\",\"target\":\"doubao\"}",
        script,argv[1],wake);
    if(n<=0||(size_t)n>=sizeof p->request||
       dc_mode_parse_request(DC_MODE_PHONE,p->request,(size_t)n,&checked))return -1;
    p->size=(size_t)n;p->active=1;return 0;
}
static int phone_channel(struct phone_sender *p) {
    if(!p->active)return 0;
    int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return -1;
    /* Never replace caller FD9 or any existing lease/light/proof descriptor. */
    for(int i=0;i<2;i++) {
        p->fd[i]=fcntl(pair[i],F_DUPFD_CLOEXEC,10);
        if(p->fd[i]<0){close(pair[0]);close(pair[1]);if(p->fd[0]>=0)close(p->fd[0]);return -1;}
    }
    close(pair[0]);close(pair[1]);return 0;
}
static int phone_send(struct phone_sender *p) {
    if(!p->active)return 1;
    ssize_t n;do{n=send(p->fd[0],p->request,p->size,MSG_DONTWAIT|MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
    return n==(ssize_t)p->size;
}
#endif
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"busy")) return lx06_session_busy() ? 0 : 1;
    if(argc==2 && !strcmp(argv[1],"preempt")) return lx06_session_preempt()<0 ? 1 : 0;
    if(argc<4) return 2;
    int local=!strcmp(argv[1],"run-local");
    if(!local && strcmp(argv[1],"run")) return 2;
    if(!lx06_session_name_valid(argv[2])) return 2;
    unsigned char epoch[LX06_IDLE_EPOCH_BYTES]; uint64_t expires=0;
    if(local && (argc<6 || local_ticket(argv[3],argv[4],epoch,&expires))) return 2;
    if(local && !local_ticket_live(expires)) return 75;
    int command_at=local ? 5 : 3;
#ifdef LX06_SESSION_PHONE_RUNTIME
    struct phone_sender phone;
    if(phone_prepare(&phone,argv[2],argc-command_at,argv+command_at))return 2;
#endif
    struct lx06_resource_issuer proof;if(resource_issuer_open(&proof))return 2;
    if(lx06_session_busy()) return 75;
    /* tmpfs recordings also consume the speaker's 128 MiB RAM. Refuse a new
     * cloud/player group without headroom, and shed only that group if pressure
     * rises. Do not wait for the kernel to kill the official audio engine. */
    /* MemAvailable includes reclaimable cache. On the LX06 4.9 kernel an
     * order-2 kernel-stack allocation has failed with 19 MiB available but
     * only 5 MiB free. Check physical reserve when reclaimable headroom is
     * also low. MemFree alone rejects healthy sessions with large file cache
     * (observed: 28 MiB available, 5.9 MiB free during normal client startup). */
    long free_kb,memory=available_kb(&free_kb);
    if(memory<20480 || free_kb<0 || (memory<24576 && free_kb<8192)) {
        fprintf(stderr,"session-owner: refused name=%s available_kb=%ld free_kb=%ld minimum_kb=20480 reserve_free_kb=8192 reserve_below_available_kb=24576\n",argv[2],memory,free_kb);
        return 125;
    }
    int lock=local ? lx06_idle_epoch_take(epoch) : lx06_session_take_idle();
    if(lock<0) return 75;
    if((local && !local_ticket_live(expires)) || lx06_idle_epoch_changed(lock)) {
        close(lock);
        return 75;
    }
    signal(SIGCHLD,SIG_DFL); signal(SIGTERM,stop); signal(SIGINT,stop); signal(SIGUSR1,stop);
    signal(SIGPIPE,SIG_IGN);
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)) return 2;
    struct light_owner light={0};
    int light_child=light_channel(&light);
    int gate[2]; if(pipe(gate)) return 2;
#ifdef LX06_SESSION_PHONE_RUNTIME
    if(phone_channel(&phone))return 2;
#endif
    pid_t supervisor=getpid(), child=fork();
    if(child<0) return 2;
    if(!child) {
#ifdef LX06_SESSION_PHONE_RUNTIME
        if(phone.active)close(phone.fd[0]);
#endif
        if(proof.fd>=0)close(proof.fd); /* no business-script issuer endpoint */
        if(light.fd>=0) close(light.fd);
        close(gate[1]);
        if(fcntl(lock,F_SETFD,0)) _exit(126);
        signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGUSR1,SIG_DFL);
        signal(SIGPIPE,SIG_DFL);
        if(setpgid(0,0)) _exit(126);
        if(prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=supervisor) _exit(126);
        char ready;
        if(read(gate[0],&ready,1)!=1) _exit(126);
        close(gate[0]);
#ifdef LX06_SESSION_PHONE_RUNTIME
        if(phone.active) {
            char fd_text[24];snprintf(fd_text,sizeof fd_text,"%d",phone.fd[1]);
            if(fcntl(phone.fd[1],F_SETFD,0))_exit(126);
            const char *core=getenv("LX06_DIALOGUED");if(!core||!*core)core="/usr/bin/lx06-dialogued";
            execl(core,core,"--phone-fd",fd_text,(char *)NULL);_exit(127);
        }
#endif
        execvp(argv[command_at],argv+command_at); _exit(127);
    }
#ifdef LX06_SESSION_PHONE_RUNTIME
    if(phone.active)close(phone.fd[1]);
#endif
    if(light_child>=0) close(light_child);
    unsetenv("LX06_DIALOG_FD");unsetenv("LX06_DIALOG_CHANNEL");
    close(gate[0]);
    (void)setpgid(child,child);
    struct lx06_session_record record={LX06_SESSION_MAGIC,(uint32_t)supervisor,(uint32_t)child,(uint32_t)lock,
                                      lx06_process_ticks(supervisor),lx06_process_ticks(child)};
    if(!record.supervisor_ticks || !record.group_ticks ||
       pwrite(lock,&record,sizeof(record),0)!=(ssize_t)sizeof(record)) stopping=SIGTERM;
    char name[LX06_SESSION_NAME_SIZE]={0};
    memcpy(name,argv[2],strlen(argv[2]));
    if(pwrite(lock,name,sizeof(name),sizeof(record))!=(ssize_t)sizeof(name) ||
       ftruncate(lock,sizeof(record)+sizeof(name))) stopping=SIGTERM;
    if(!stopping&&!resource_issuer_start(&proof,lock,&record,name))stopping=SIGTERM;
#ifdef LX06_SESSION_PHONE_RUNTIME
    if(!stopping&&!phone_send(&phone))stopping=SIGTERM;
    if(phone.active)close(phone.fd[0]);
#endif
    if(!stopping) (void)write(gate[1],"1",1);
    close(gate[1]);
    fprintf(stderr,"session-owner: start name=%s supervisor=%d group=%d\n",argv[2],supervisor,child);
    long long started=tick_ms(), terminate_at=0, killed_at=0;
    const char *max_text=getenv("LX06_SESSION_MAX_SECONDS");
    long seconds=max_text ? strtol(max_text,NULL,10) : 900;
    if(seconds<1 || seconds>86400) seconds=900;
    int root_done=0, root_status=0, requested=0, timed_out=0, memory_pressure=0;
    long long next_memory=started;
    for(;;) {
        resource_issuer_tick(&proof,lock);
        int status;
        pid_t done;
        while((done=waitpid(-1,&status,WNOHANG))>0) {
            if(done==child) { root_done=1;root_status=status; }
            else light_reaped(&light,done,status);
        }
        int wait_error=errno;
        int descendants_reaped=done<0&&wait_error==ECHILD;
        if(proof.fd>=0){if(root_done&&descendants_reaped)break;}
        else if(root_done && kill(-child,0)<0 && errno==ESRCH && lx06_orphan_signal(&record,0)==0) break;
        long long now=tick_ms();
        if(!terminate_at && !stopping && !root_done) light_tick(&light,lock,now);
        if(!terminate_at && !stopping && !root_done && now>=next_memory) {
            next_memory=now+250;
            memory=available_kb(&free_kb);
            if(memory<12288 || free_kb<0 || (memory<24576 && free_kb<6144)) {
                memory_pressure=1;
                fprintf(stderr,"session-owner: memory pressure name=%s available_kb=%ld free_kb=%ld minimum_kb=12288 reserve_free_kb=6144 reserve_below_available_kb=24576\n",argv[2],memory,free_kb);
            }
        }
        if(!terminate_at && (stopping || root_done || memory_pressure || now-started>=seconds*1000LL)) {
            timed_out=!stopping && !root_done && now-started>=seconds*1000LL;
            requested=stopping || !root_done;
            terminate_at=now;
            if(proof.fd<0||!root_done)kill(-child,SIGTERM);
            if(proof.fd>=0&&!resource_issuer_signal_children(SIGTERM))proof.invalid=1;
        }
        if(terminate_at && now-terminate_at>=400 && now-killed_at>=100) {
            if(proof.fd<0){kill(-child,SIGKILL);(void)lx06_orphan_signal(&record,SIGKILL);}
            else {
                if(!root_done)kill(-child,SIGKILL);
                if(!resource_issuer_signal_children(SIGKILL))proof.invalid=1;
            }
            killed_at=now;
        }
        usleep(10000);
    }
    light_close(&light,lock);
    /* Zero metadata while the lock is still held; a stale PID can never target
     * a later process after normal exit. Keep the inode for other contenders. */
    if(proof.fd<0){(void)ftruncate(lock,0);close(lock);} /* unchanged legacy */
    else resource_issuer_finish(&proof,lock);
    int rc=memory_pressure ? 125 : timed_out ? 124 : requested ? 0 : WIFEXITED(root_status) ? WEXITSTATUS(root_status) : 128+WTERMSIG(root_status);
    fprintf(stderr,"session-owner: closed name=%s reason=%d status=%d\n",argv[2],(int)stopping,rc);
    return rc;
}
