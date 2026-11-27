/* Included by session-run after tick_ms(). No extra persistent process, files,
 * global socket or DATA writes. LED workers never inherit the session lease. */
#include "dialog-light.h"
#include <sys/file.h>
#include <sys/stat.h>
#ifndef LX06_LED_UBUS
#define LX06_LED_UBUS "/bin/ubus"
#endif
#ifndef LX06_SILENT_FILE
#define LX06_SILENT_FILE "/tmp/silent.flag"
#endif
#ifndef LX06_LIGHT_CHECK_MS
#define LX06_LIGHT_CHECK_MS 1000
#endif
#ifndef LX06_LIGHT_LEASE_DIR
#define LX06_LIGHT_LEASE_DIR "/tmp/lx06-dialog-led"
#endif
struct light_owner {
    int fd, desired, applied, closing, failed, obsolete;
    int status_fd, lease_fd;
    pid_t worker;
    long long deadline, next_check;
};
static int light_lease(void) {
    /* The stock shell dispatcher shares this lock for native cleanup.
     * Hold it until our own lights are cleared, then let native calls resume. */
    if(mkdir(LX06_LIGHT_LEASE_DIR,0700) && errno!=EEXIST) return -1;
    int directory=open(LX06_LIGHT_LEASE_DIR,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0) return -1;
    struct stat info;
    int fd=-1;
    if(!fstat(directory,&info) && info.st_uid==geteuid() && !(info.st_mode&0077))
        fd=openat(directory,"lease",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    close(directory);
    if(fd<0) return -1;
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() ||
       flock(fd,LOCK_EX|LOCK_NB)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int light_silent(void) {
    char flag=0;
    int fd=open(LX06_SILENT_FILE,O_RDONLY|O_CLOEXEC);
    if(fd>=0) { (void)read(fd,&flag,1);close(fd); }
    return flag=='1';
}
static void light_command(int show,int effect) {
    /* Same silent gate as stock show_led, without a shell/cat/logger process
     * tree for every state. ledserver still owns all LX06/night animations. */
    if(show && light_silent()) return;
    pid_t parent=getpid(), pid=fork();
    if(!pid) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(1);
        char message[32];snprintf(message,sizeof(message),"{\"L\":%d}",effect);
        int null=open("/dev/null",O_WRONLY);if(null>=0) {dup2(null,1);close(null);}
        execl(LX06_LED_UBUS,LX06_LED_UBUS,"-t","1","call","led",show?"show":"shut",message,(char *)NULL);_exit(127);
    }
    if(pid<0) _exit(1);
    int status;
    while(waitpid(pid,&status,0)<0) if(errno!=EINTR) _exit(1);
    if(!WIFEXITED(status) || WEXITSTATUS(status)) _exit(1);
}
static int light_effect(int state) {
    if(state<0) return -1;
    return state==LX06_LIGHT_THINKING ? 2 : state==LX06_LIGHT_SPEAKING ? 3 :
           state==LX06_LIGHT_END ? 0 : 1;
}
static void light_drive(int state) {
    /* 1=official beam/listen, 2=think, 3=player, 11=ready/end.
     * No angle is invented: ledserver uses its retained/default position.
     * Only dialogue effects are touched; never call shut_all. */
    int effect=light_effect(state);
    const int effects[]={1,2,3,9,11};
    if(state==LX06_LIGHT_END) light_command(1,11);
    if(effect) {
        /* Keep the current dialogue effect until show(new) actually succeeds.
         * Never clear the target first: a delayed/failed show must not leave
         * an active conversation dark. Same-effect changes are coalesced by
         * light_tick; a failed retry may show again, but must not first shut
         * an effect which can still be the only visible session indicator. */
        light_command(1,effect);
    }
    for(unsigned i=0;i<sizeof(effects)/sizeof(effects[0]);i++)
        if(effects[i]!=effect) light_command(0,effects[i]);
}
static int light_channel(struct light_owner *light) {
    light->fd=-1;light->status_fd=-1;light->lease_fd=-1;
    light->desired=LX06_LIGHT_WAKE;light->applied=-1;
    const char *on=getenv("LX06_DIALOG_LIGHTS");
    unsetenv("LX06_DIALOG_FD");unsetenv("LX06_DIALOG_CHANNEL");
    if(!on || strcmp(on,"1")) return -1;
    int sockets[2];
    if(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,sockets)) return -1;
    /* Bound queued updates; producer always drops instead of blocking audio. */
    int capacity=1024;
    setsockopt(sockets[1],SOL_SOCKET,SO_SNDBUF,&capacity,sizeof(capacity));
    char fdtext[16],path[64],identity[96];
    snprintf(fdtext,sizeof(fdtext),"%d",sockets[1]);
    snprintf(path,sizeof(path),"/proc/self/fd/%d",sockets[1]);
    ssize_t n=readlink(path,identity,sizeof(identity)-1);
    if(n<=0 || fcntl(sockets[1],F_SETFD,0)) {close(sockets[0]);close(sockets[1]);return -1;}
    identity[n]=0;
    setenv("LX06_DIALOG_FD",fdtext,1);setenv("LX06_DIALOG_CHANNEL",identity,1);
    light->fd=sockets[0];return sockets[1];
}
/* Stock led status wraps the effects JSON in an escaped string. A successful
 * empty info means the effect list was cleared. A truncated/unknown response
 * is not evidence of a missing effect. */
static int light_status_missing(int fd,int effect) {
    char status[4096];
    size_t used=0;
    while(used<sizeof(status)-1) {
        ssize_t n=read(fd,status+used,sizeof(status)-1-used);
        if(n<0 && errno==EINTR) continue;
        if(n<0) return 0;
        if(!n) break;
        used+=(size_t)n;
    }
    if(used==sizeof(status)-1) return 0;
    status[used]=0;
    const char *info=strstr(status,"\"info\"");
    if(!info) return 0;
    const char *value=strchr(info+6,':');
    if(!value) return 0;
    value+=1+strspn(value+1," \t\r\n");
    if(*value!='"') return 0;
    if(value[1]=='"') return 1;
    if(!strstr(value,"\\\"effect\\\"")) return 0;
    const char *type=value;
    while((type=strstr(type,"\\\"type\\\""))) {
        type+=8;
        const char *separator=type+strspn(type," \t");
        if(*separator==':' && strtol(separator+1,NULL,10)==effect) return 0;
    }
    return 1;
}
static void light_reaped(struct light_owner *light,pid_t pid,int status) {
    if(pid!=light->worker) return;
    /* The worker waits for its direct ubus child. Timeout/preempt kills the
     * group before reaping, while its PID is still reserved; never signal a
     * potentially recycled PGID after waitpid has released the leader. */
    light->worker=0;
    int obsolete=light->obsolete;light->obsolete=0;
    int ok=WIFEXITED(status) && !WEXITSTATUS(status);
    long long now=tick_ms();
    light->next_check=now+LX06_LIGHT_CHECK_MS;
    if(light->status_fd>=0) {
        if(!obsolete && ok && light_status_missing(light->status_fd,light_effect(light->applied))) {
            light->failed=2;
            light->next_check=now;
        }
        close(light->status_fd);light->status_fd=-1;
    } else if(!obsolete && !ok) {
        light->failed=1;
        fprintf(stderr,"dialog-light: backend failed state=%s status=%d\n",lx06_light_name(light->applied),status);
    }
}
static void light_tick(struct light_owner *light,int lock,long long now) {
    if(light->fd<0) return;
    unsigned char message[2];
    for(int i=0;i<64;i++) {
        ssize_t n=recv(light->fd,message,sizeof(message),MSG_DONTWAIT);
        if(n<0) break;
        /* Clients may finish individual audio segments, but that is not a
         * session close. Only light_close, after session-run has reaped every
         * session descendant, is allowed to end the dialogue illumination. */
        if(!light->closing && n==1 && message[0]>LX06_LIGHT_END && message[0]<LX06_LIGHT_COUNT)
            light->desired=message[0];
    }
    if(light->worker) {
        if(!light->closing && light_effect(light->desired)!=light_effect(light->applied)) {
            /* A bounded slow ubus operation from the previous state must not
             * finish after the latest state is installed. Reap the old group
             * before starting a replacement. Its leader PID is still reserved
             * here, so neither a recycled PID nor another session is signaled.
             * show-before-shut leaves a valid effect lit during cancellation. */
            light->obsolete=1;
            kill(-light->worker,SIGKILL);
        } else if(now>=light->deadline) kill(-light->worker,SIGKILL);
        return;
    }
    if(light->lease_fd<0) {
        if(light->closing || now<light->next_check) return;
        light->lease_fd=light_lease();
        if(light->lease_fd<0) {light->next_check=now+100;return;}
    }
    int same_effect=light_effect(light->desired)==light_effect(light->applied);
    if(light->desired!=light->applied && !light->failed && same_effect) {
        light->applied=light->desired;
        fprintf(stderr,"dialog-light: state=%s\n",lx06_light_name(light->desired));
    }
    int stable=light->desired==light->applied;
    if(stable && (light->closing || now<light->next_check)) return;
    if(stable && light_silent()) {
        light->next_check=now+LX06_LIGHT_CHECK_MS;
        return;
    }
    int probe=stable && !light->failed;
    int repair=stable && light->failed==2;
    int output[2]={-1,-1};
    if(probe && pipe2(output,O_CLOEXEC|O_NONBLOCK)) {
        light->next_check=now+LX06_LIGHT_CHECK_MS;
        return;
    }
    pid_t parent=getpid(),pid=fork();
    if(!pid) {
        close(lock);close(light->fd);
        close(light->lease_fd);
        signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGUSR1,SIG_DFL);
        if(setpgid(0,0) || prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(1);
        setenv("LED_PARENT","lx06-dialog",1);
        if(probe) {
            close(output[0]);
            if(dup2(output[1],STDOUT_FILENO)<0) _exit(1);
            close(output[1]);
            execl(LX06_LED_UBUS,LX06_LED_UBUS,"-t","1","call","led","status",(char *)NULL);
            _exit(127);
        }
        if(repair) { light_command(1,light_effect(light->desired));_exit(0); }
        light_drive(light->desired);_exit(0);
    }
    if(probe) close(output[1]);
    if(pid<0) {
        if(probe) close(output[0]);
        light->next_check=now+LX06_LIGHT_CHECK_MS;
        return;
    }
    (void)setpgid(pid,pid);
    light->status_fd=output[0];
    light->worker=pid;light->deadline=now+1800;light->applied=light->desired;light->failed=0;
    if(repair) fprintf(stderr,"dialog-light: restore=%s\n",lx06_light_name(light->desired));
    else if(!probe) fprintf(stderr,"dialog-light: state=%s\n",lx06_light_name(light->desired));
}
static void light_close(struct light_owner *light,int lock) {
    if(light->fd<0) return;
    int clean=!light->worker && !light->failed && light->applied==LX06_LIGHT_END;
    light->closing=1;light->desired=LX06_LIGHT_END;
    if(light->worker) {
        kill(-light->worker,SIGKILL);
        int status;pid_t pid;
        do {pid=waitpid(light->worker,&status,0);} while(pid<0 && errno==EINTR);
        if(pid>0) light_reaped(light,pid,status);
        light->worker=0;
    }
    /* Force cleanup after an interrupted in-flight operation, even if an
     * earlier explicit ending already ran. Never wait indefinitely on ubus. */
    if(!clean) light->applied=-1;
    light_tick(light,lock,tick_ms());
    while(light->worker) {
        int status;pid_t pid=waitpid(light->worker,&status,WNOHANG);
        if(pid>0) light_reaped(light,pid,status);
        else if(pid<0 && errno==ECHILD) light->worker=0;
        else {light_tick(light,lock,tick_ms());usleep(10000);}
    }
    /* All remaining descendants are killed LED helpers; reap them before a
     * new owner is allowed to show its wake light. */
    for(;;) {
        pid_t pid=waitpid(-1,NULL,0);
        if(pid>0 || (pid<0 && errno==EINTR)) continue;
        break;
    }
    close(light->fd);light->fd=-1;
    if(light->lease_fd>=0) close(light->lease_fd);
    light->lease_fd=-1;
}
