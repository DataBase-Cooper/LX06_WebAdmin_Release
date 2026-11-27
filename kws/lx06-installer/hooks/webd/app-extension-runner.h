/* Executed by keeper in a SEPARATE job, under the script's declared privilege.
 * No credentials on argv, in keeper logs, or in the public status document.
 * This runner bounds time/output itself; keeper timeout would latch safe mode. */
#include "app-extension-options.h"
#include "../../runtime/common/script-exec-source-v1.h"
static volatile sig_atomic_t ext_stopping;
static int ext_values_updated;
#ifndef EXT_TIMEOUT_MS
#define EXT_TIMEOUT_MS 300000
#endif
static void ext_signal(int sig) { (void)sig; ext_stopping=1; }
static int ext_save_state(int dir,int running,int exitcode,const char*event) {
    char out[EXT_EVENT_CAP+256];
    int n=snprintf(out,sizeof(out),"{\"running\":%s,\"exit\":%d,\"values_updated\":%s,\"event\":%s}",
                   running?"true":"false",exitcode,ext_values_updated?"true":"false",event);
    if(n<0||(size_t)n>=sizeof(out))return -1;
    unlinkat(dir,"state.new",0);
    int f=openat(dir,"state.new",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(f<0)return -1;
    int rc=write_all(f,out,(size_t)n); close(f);
    if(rc||renameat(dir,"state.new",dir,"state.json"))return -1;
    return 0;
}
int webd_extension_main(int argc,char**argv) {
    ext_values_updated=0;
    if(argc!=5&&argc!=6)return 2;
    int bound=argc==6,source_fd=-1,source_bad=0;
    const char *path=argv[2],*script=argv[3],*action=argv[4];
    if(strncmp(path,"/tmp/lx06-extensions/ext-",strlen("/tmp/lx06-extensions/ext-"))||
       strncmp(script,APPPERSIST "/",strlen(APPPERSIST)+1)||!ext_ident(action))return 2;
    /* q6 is the explicit trusted-owner form. Reserve before this runner's
     * own input/cache/pipe opens, and never overwrite a caller-owned FD9.
     * Source refusal remains a business result, not keeper quarantine. */
    if(bound) {
        if(strnlen(argv[5],EXT_EXEC_SOURCE_CAP)>=EXT_EXEC_SOURCE_CAP||
           strncmp(argv[5],"shell-fd-v1:",12)||
           fcntl(EXT_EXEC_SOURCE_FD,F_GETFD)>=0||errno!=EBADF)source_bad=1;
        else {
            source_fd=open(script,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(source_fd<0)source_bad=1;
            else if(source_fd!=EXT_EXEC_SOURCE_FD) {
                if(dup2(source_fd,EXT_EXEC_SOURCE_FD)<0)source_bad=1;
                close(source_fd);source_fd=source_bad?-1:EXT_EXEC_SOURCE_FD;
            }
            if(source_fd>=0&&(fcntl(source_fd,F_SETFD,FD_CLOEXEC)||
               !ext_exec_source_current(source_fd,script,argv[5])||
               ext_exec_source_contract(source_fd)!=1))source_bad=1;
        }
    }
    int dir=-1,in=-1,p[2],status=0,rc=125; pid_t pid=-1;
    int64_t until=lk_now_ms()+5000;
    signal(SIGTERM,ext_signal); signal(SIGINT,ext_signal); signal(SIGPIPE,SIG_IGN);
    signal(SIGCHLD,SIG_DFL);
    while(!ext_stopping&&lk_now_ms()<until) {
        dir=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir>=0) { in=openat(dir,"input.json",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); if(in>=0)break; close(dir);dir=-1; }
        poll(NULL,0,50);
    }
    if(in<0){if(source_fd>=0)close(source_fd);return 0;}
    LX06_STATBUF st;
    if(LX06_FSTAT(in,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_nlink!=1||
       (st.st_mode&0777)!=0600||st.st_size>EXT_CAP) {close(in);close(dir);if(source_fd>=0)close(source_fd);return 0;}
    /* Remove the only pathname before executing a third-party script. */
    unlinkat(dir,"input.json",0);
    struct ext_schema *schema=calloc(1,sizeof(*schema));
    int sf=bound?source_fd:open(script,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(!schema||sf<0||ext_read(sf,schema)!=1){free(schema);schema=NULL;}
    if(!bound&&sf>=0)close(sf);
    if(bound&&(source_bad||!schema||!ext_exec_source_current(sf,script,argv[5])||
       ext_exec_source_contract(sf)!=1)) {
        free(schema);ext_save_state(dir,0,65,"{\"message\":\"扩展脚本来源已变化或执行声明无效\"}");
        close(in);close(dir);if(source_fd>=0)close(source_fd);return 0;
    }
    if(schema) {
        /* A reset left by an earlier direct helper call applies to the old
         * action only; this new action may fetch a fresh catalogue. */
        if(unlinkat(dir,"dynamic.reset",0)&&errno!=ENOENT){free(schema);close(in);close(dir);if(source_fd>=0)close(source_fd);return 0;}
        (void)ext_cache_load(schema,dir);
        char request[EXT_CAP+1],checked[33];ssize_t n=read(in,request,sizeof(request)-1);
        if(n<0||n>EXT_CAP){free(schema);close(in);close(dir);if(source_fd>=0)close(source_fd);return 0;}
        request[n]=0;
        if(memchr(request,0,(size_t)n)||!ext_request(schema,request,checked)||strcmp(checked,action)||lseek(in,0,SEEK_SET)<0) {
            free(schema);ext_save_state(dir,0,65,"{\"message\":\"扩展参数或模型目录已变化，请重新读取配置\"}");close(in);close(dir);if(source_fd>=0)close(source_fd);return 0;
        }
    }
    char event[EXT_EVENT_CAP+1]="{\"message\":\"处理中…\"}";
    char pending_options[EXT_EVENT_CAP+1]="",pending_current[EXT_EVENT_CAP+1]="";
    if(ext_save_state(dir,1,0,event)||pipe2(p,O_CLOEXEC))goto done;
    pid=fork();
    if(pid<0){close(p[0]);close(p[1]);goto done;}
    if(!pid) {
        setpgid(0,0); signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL);
        dup2(in,0); dup2(p[1],1);
        if(setenv("LX06_EXTENSION_DIR",path,1))_exit(127);
        int nullfd=open("/dev/null",O_WRONLY); if(nullfd>=0)dup2(nullfd,2);
        close(p[0]);close(p[1]);close(in);close(dir);
        if (!strcmp(action,"regenerate-greeting")) {
            if (!schema || ext_greeting_field(schema)<0) _exit(65);
            execl("/usr/lib/lx06/script-settings","script-settings","greeting",script,(char *)NULL);
            _exit(127);
        }
        if(bound) {
            if(!ext_exec_source_current(source_fd,script,argv[5])||
               ext_exec_source_contract(source_fd)!=1||lseek(source_fd,0,SEEK_SET)<0||
               !ext_exec_source_environment(script)||fcntl(source_fd,F_SETFD,0))_exit(126);
            execl("/bin/sh","sh","/proc/self/fd/9","--web-action",action,(char*)NULL);_exit(127);
        }
        execl("/bin/sh","sh",script,"--web-action",action,(char*)NULL);_exit(127);
    }
    setpgid(pid,pid);close(p[1]);close(in);in=-1;
    if(source_fd>=0){close(source_fd);source_fd=-1;}
    fcntl(p[0],F_SETFL,O_NONBLOCK);
    until=lk_now_ms()+EXT_TIMEOUT_MS; /* includes interactive login; retry is explicit */
    size_t used=0,total=0; char line[EXT_EVENT_CAP+1]; int exited=0,bad=0,reset_seen=0;
    while(!ext_stopping&&lk_now_ms()<until&&!bad) {
        if(waitpid(pid,&status,WNOHANG)==pid)exited=1;
        struct pollfd pf={p[0],POLLIN,0}; poll(&pf,1,100);
        char b[1024];ssize_t n;
        while((n=read(p[0],b,sizeof(b)))>0) {
            total+=(size_t)n; if(total>65536){bad=1;break;}
            for(ssize_t i=0;i<n;i++) {
                if(b[i]=='\n') {
                    line[used]=0;
                    if(!ext_public_schema(line,schema)){bad=1;break;}
                    if(schema&&ext_has_dynamic(schema)) {
                        char merged[EXT_EVENT_CAP+1];
                        if(!reset_seen && ext_event_has(line,"options")) {
                            int reset_now=ext_cache_reset_requested(dir);
                            if(reset_now<0){bad=1;break;}
                            if(reset_now){
                                reset_seen=1;
                                pending_options[0]=pending_current[0]=0;
                                schema->options_json[0]=schema->current_json[0]=0;
                            }
                        }
                        if(ext_event_has(line,"options")) {
                            if(ext_cache_merge(schema,"options",pending_options[0]?pending_options:schema->options_json,line,merged)){bad=1;break;}
                            strcpy(pending_options,merged);
                        }
                        if(ext_event_has(line,"values")) {
                            if(ext_cache_merge(schema,"values",pending_current[0]?pending_current:schema->current_json,line,merged)){bad=1;break;}
                            strcpy(pending_current,merged);
                        }
                    }
                    strcpy(event,line); if(ext_save_state(dir,1,0,event)){bad=1;break;} used=0;
                } else if(!b[i]||used>=sizeof(line)-1){bad=1;break;}
                else line[used++]=b[i];
            }
            if(bad)break;
        }
        if(exited) {
            if(used)bad=1;
            rc=WIFEXITED(status)?WEXITSTATUS(status):128;
            break;
        }
    }
    close(p[0]);
    /* Always reap/stop the action group, including descendants after the shell
     * exits. Background services belong in the main script, not UI actions. */
    kill(-pid,SIGTERM);
    if(!exited) {
        int64_t end=lk_now_ms()+700;
        while(lk_now_ms()<end&&waitpid(pid,&status,WNOHANG)==0)poll(NULL,0,30);
    }
    kill(-pid,SIGKILL);if(!exited)waitpid(pid,&status,0);
    if(bad) {rc=65;strcpy(event,"{\"message\":\"脚本扩展输出格式错误或超过限制\"}");}
    else if(!exited) {rc=ext_stopping?130:124;strcpy(event,"{\"message\":\"扩展操作已停止或超时，请重试\"}");}
    int reset=ext_cache_reset_requested(dir);
    if(reset>0)unlinkat(dir,"dynamic.reset",0);
    if(rc==0&&schema&&(reset<0||((!reset||reset_seen)&&(pending_options[0]||pending_current[0])&&ext_cache_commit(schema,dir,pending_options,pending_current)))) {
        rc=74;strcpy(event,"{\"message\":\"模型目录暂存失败，原配置和目录保留，请重试\"}");
    }
    if(rc==0&&(!reset||reset_seen)&&pending_current[0])ext_values_updated=1;
done:
    if(source_fd>=0)close(source_fd);
    free(schema);
    if(in>=0)close(in);
    ext_save_state(dir,0,rc,event);close(dir);
    /* A rejected action is a UI result, not a crashed system service. Keeper
     * quarantines ALL jobs on a nonzero exit, so return success after cleanup. */
    return 0;
}
