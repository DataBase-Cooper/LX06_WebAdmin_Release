/* Provider-neutral, schema-driven settings. Scripts own defaults, controls and
 * environment mappings. C owns bounded JSON, atomic persistence and exec.
 * Password values never leave through show/save output; blank means retain.
 * run overlays saved values onto the existing script environment only at a new
 * session/client launch. No eval, shell sourcing, daemon, or live cancellation. */
#define _GNU_SOURCE
#define SETTINGS_LEGACY_ABI 1
#include "../../hooks/webd/app-extension.h"
#include "../common/script-settings-store.h"
#include "../../hooks/webd/app-extension-options.h"
#include <sys/types.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#define GREETING_STAT SETTINGS_STAT
#define GREETING_FSTAT SETTINGS_FSTAT
#include "../common/script-greeting.h"
/* Opt-in shell-fd-v1 only. Legacy run remains an arbitrary-command overlay.
 * The interpreter opens /proc/self/fd/9; the required first command closes 9
 * after that open. Scripts use explicit named identity, never FD-path $0.
 * This retains the source inode across atomic replacement, not a sealed byte
 * snapshot against an owner modifying that inode after the final check. */
#define SCRIPT_EXEC_FD 9
#define SCRIPT_EXEC_MAX_ARGS 128
#define SCRIPT_EXEC_MAX_ARG_BYTES 65536u
#define SCRIPT_EXEC_MAX_BYTES (1024u*1024u)

static int script_exec_arguments(int argc,char **argv) {
    if(argc<3||argc-3>SCRIPT_EXEC_MAX_ARGS||argv[2][0]!='/')return 0;
    size_t bytes=0;
    for(int i=2;i<argc;i++){
        size_t n=strlen(argv[i])+1;
        if(n>SCRIPT_EXEC_MAX_ARG_BYTES-bytes)return 0;
        bytes+=n;
    }
    return 1;
}
static int script_exec_valid_stat(const SETTINGS_STAT *s) {
    return S_ISREG(s->st_mode)&&s->st_uid==getuid()&&s->st_nlink==1&&
        !(s->st_mode&0022)&&s->st_size>0&&(uint64_t)s->st_size<=SCRIPT_EXEC_MAX_BYTES;
}
static int script_exec_same(const SETTINGS_STAT *a,const SETTINGS_STAT *b) {
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&
        a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&a->st_nlink==b->st_nlink&&a->st_size==b->st_size&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int script_exec_current(int fd,const char *path,const SETTINGS_STAT *before) {
    SETTINGS_STAT held,named;int fresh=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    int ok=fresh>=0&&!SETTINGS_FSTAT(fd,&held)&&!SETTINGS_FSTAT(fresh,&named)&&
        script_exec_valid_stat(&held)&&script_exec_same(before,&held)&&script_exec_same(&held,&named);
    if(fresh>=0)close(fresh);
    return ok;
}
static int script_exec_contract(int fd) {
    char bytes[16385];ssize_t n;
    do {n=pread(fd,bytes,sizeof(bytes)-1,0);} while(n<0&&errno==EINTR);
    if(n<=0||memchr(bytes,0,(size_t)n))return 0;
    bytes[n]=0;
    char *line=bytes;int marker=0,first=1;
    for(;;){
        char *end=strchr(line,'\n');if(!end)return 0;*end=0;
        if(first){if(strcmp(line,"#!/bin/sh"))return 0;first=0;}
        else{
            while(*line==' '||*line=='\t')line++;
            if(!strncmp(line,"# LX06-EXEC:",12)){
                if(marker||strcmp(line,"# LX06-EXEC: shell-fd-v1"))return 0;
                marker=1;
            }else if(*line&&*line!='#')return marker&&!strcmp(line,"exec 9<&-");
        }
        line=end+1;
    }
}
static int script_exec_environment(const char *path,const char *name) {
    return !setenv("LX06_SCRIPT_EXEC_PROTOCOL","shell-fd-v1",1)&&
        !setenv("LX06_SCRIPT_PATH",path,1)&&!setenv("LX06_SCRIPT_ID",name,1);
}


static struct ext_schema schema;
static char values[EXT_FIELDS][2049];
static int present[EXT_FIELDS];
static unsigned restore_mask;
static int retained_unknown;
static int active_settings_lock=-1;
static char stored_record[SETTINGS_CAP+1]="{}";
static char output[EXT_EVENT_CAP+1];static size_t used;

#ifndef LX06_GREETING_SYNTH
#define LX06_GREETING_SYNTH "/usr/bin/lx06-model-audio"
#endif

static const char *configured_greeting(void) {
    int field = ext_greeting_field(&schema);
    return field >= 0 && present[field] ? values[field] : NULL;
}

static int greeting_write(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n;
        size -= (size_t)n;
    }
    return 0;
}

static int synthesize_greeting(const char *text, int wav) {
    int input[2], status;
    if (pipe2(input, O_CLOEXEC)) return -1;
    if (greeting_write(input[1], text, strlen(text))) {
        close(input[0]); close(input[1]); return -1;
    }
    close(input[1]);
    pid_t child = fork();
    if (!child) {
        struct rlimit limit = {GREETING_WAV_MAX, GREETING_WAV_MAX};
        if (dup2(input[0], STDIN_FILENO) < 0 || dup2(wav, STDOUT_FILENO) < 0 ||
            setrlimit(RLIMIT_FSIZE, &limit)) _exit(126);
        execl(LX06_GREETING_SYNTH, LX06_GREETING_SYNTH, "--synthesize-fresh", (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    if (child < 0) return -1;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Synthesis waits outside the settings lock. Revalidate source and settings
 * before publishing so a concurrent save, upload or uninstall wins. */
static int refresh_greeting(const char *script, const char *name, const char *text,
                            const SETTINGS_STAT *revision, int settings, int *lock) {
    if (!text || strlen(text) >= GREETING_TEXT_BYTES) return text ? -1 : 0;
    if (!*text) {
        int dir = greeting_script_dir(name, 0);
        if (dir < 0) return errno == ENOENT ? 0 : -1;
        int result = (unlinkat(dir, GREETING_FILE, 0) && errno != ENOENT) ? -1 : fsync(dir);
        close(dir);
        return result;
    }
    char before[SETTINGS_CAP+1], after[SETTINGS_CAP+1];
    int saved = settings_read(settings,name,before);
    if(saved<0)return -1;
    struct statvfs space;
    if (fstatvfs(settings, &space) || (uint64_t)space.f_bavail * space.f_frsize < 1152u * 1024u)
        return -1;
    char path[] = "/tmp/lx06-greeting.XXXXXX";
    int wav = mkostemp(path, O_CLOEXEC);
    if (wav < 0) return -1;
    unlink(path);
    close(*lock);
    *lock=active_settings_lock=-1;
    int dir=-1,result=-1;
    unsigned rate, channels, bytes;
    if (synthesize_greeting(text, wav) || !greeting_wave(wav, 0, &rate, &channels, &bytes))
        goto done;
    for (int attempt=0;;attempt++) {
        *lock=settings_lock_acquire(settings);
        if(*lock>=0)break;
        if(errno!=EBUSY||attempt==19)goto done;
        usleep(100000);
    }
    active_settings_lock=*lock;
    SETTINGS_STAT latest;
    int current=open(script,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    int same=current>=0&&!SETTINGS_FSTAT(current,&latest)&&script_exec_same(revision,&latest);
    if(current>=0)close(current);
    if(!same||settings_read(settings,name,after)!=saved||(saved&&strcmp(before,after)))
        goto done;
    if(fstatvfs(settings,&space)||(uint64_t)space.f_bavail*space.f_frsize<1152u*1024u)
        goto done;
    dir=greeting_script_dir(name,1);
    if(dir<0)goto done;

    unlinkat(dir, ".pending", 0);
    int out = openat(dir, ".pending", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) goto done;
    char stored[GREETING_TEXT_BYTES] = {0}, buffer[4096];
    memcpy(stored, text, strlen(text));
    int complete = !greeting_write(out, stored, sizeof stored);
    for (off_t offset = 0, end = (off_t)bytes + 44; complete && offset < end;) {
        size_t want = end - offset < (off_t)sizeof buffer ? (size_t)(end - offset) : sizeof buffer;
        ssize_t n = pread(wav, buffer, want, offset);
        if (n < 0 && errno == EINTR) continue;
        complete = n > 0 && !greeting_write(out, buffer, (size_t)n);
        if (complete) offset += n;
    }
    if (complete && !fsync(out) && !renameat(dir, ".pending", dir, GREETING_FILE))
        result = fsync(dir);
    close(out);
    unlinkat(dir, ".pending", 0);
done:
    close(wav);
    if(dir>=0)close(dir);
    return result;
}

static int append(const char*s) {
    size_t n=strlen(s);if(n+used>=sizeof(output))return -1;
    memcpy(output+used,s,n+1);used+=n;return 0;
}
static int quoted(const char*s) {
    if(append("\""))return -1;
    for(;*s;s++) {
        char b[8];unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){b[0]='\\';b[1]=c;b[2]=0;}
        else if(c<32)snprintf(b,sizeof(b),"\\u%04x",c);
        else {b[0]=c;b[1]=0;}
        if(append(b))return -1;
    }
    return append("\"");
}
static int object(int public) {
    int count=0;if(append("{"))return -1;
    for(int i=0;i<schema.nf;i++) {
        if(!present[i]||(public&&!strcmp(schema.fields[i].type,"password")))continue;
        if((count++&&append(","))||quoted(schema.fields[i].name)||append(":")||quoted(values[i]))return -1;
    }
    if(!public) {
        xz_json_doc d;if(xz_json_parse(&d,stored_record,strlen(stored_record)))return -1;
        for(int k=d.tokens[0].first_child;k>=0;) {
            char key[33],v[SETTINGS_CAP+1];int t=d.tokens[k].next_sibling;
            if(decode_string(&d,k,key,sizeof(key))||!ext_text(&d,0,key,v,sizeof(v),0))return -1;
            if(ext_field_index(&schema,key)<0&&((count++&&append(","))||quoted(key)||append(":")||quoted(v)))return -1;
            k=d.tokens[t].next_sibling;
        }
    }
    return append("}");
}
static int message(const char*s,int rc) {
    used=0;append("{\"message\":");quoted(s);append("}");puts(output);return rc;
}
static int read_stored(int dir,const char*name) {
    strcpy(stored_record,"{}");int rc=dir<0?0:settings_read(dir,name,stored_record);
    if(rc<0||ext_settings_decode_ignoring(&schema,stored_record,restore_mask))return -1;
    xz_json_doc d;if(xz_json_parse(&d,stored_record,strlen(stored_record)))return -1;
    for(int k=d.tokens[0].first_child;k>=0;k=d.tokens[d.tokens[k].next_sibling].next_sibling) {
        char key[33];if(decode_string(&d,k,key,sizeof(key)))return -1;if(ext_field_index(&schema,key)<0)retained_unknown=1;
    }
    for(int f=0;f<schema.nf;f++)if(schema.saved_mask&(1U<<f)) {
        strcpy(values[f],schema.saved_values[f]);present[f]=1;
        if(schema.fields[f].dynamic){strcpy(schema.current[f],values[f]);schema.current_mask|=1U<<f;}
    }return rc;
}
static int defaults(void) {
    for(int i=0;i<schema.nf;i++) {
        if(present[i])continue;
        char env[65];const char *v=NULL;
        if(!ext_text(&schema.doc,schema.fields[i].token,"env",env,sizeof(env),0))return -1;
        if(*env)v=getenv(env);
        if(v) {
            if(!ext_value(&schema,i,v))return -1;
            strcpy(values[i],v);present[i]=1;
        } else if(ext_effective_value(&schema,i,values[i])) {
            present[i]=1;
        }
    }
    return 0;
}
static int commit_record(int dir,const char*name);
static int save(int dir,const char*name) {
    char raw[EXT_CAP+2],action[33];size_t n=fread(raw,1,sizeof(raw)-1,stdin);raw[n]=0;
    if(n>EXT_CAP||memchr(raw,0,n)||!ext_request(&schema,raw,action)||strcmp(action,"save"))return -1;
    xz_json_doc d;if(xz_json_parse(&d,raw,n))return -1;
    int vs=xz_json_object_get(&d,0,"values");
    for(int i=0;i<schema.nf;i++)if(xz_json_object_get(&d,vs,schema.fields[i].name)>=0) {
        char b[2049];if(!ext_text(&d,vs,schema.fields[i].name,b,sizeof(b),0))return -1;
        if(!*b&&!strcmp(schema.fields[i].type,"password"))continue;
        if(!ext_rows_edit(&schema,i,b,values[i]))return -1;
        present[i]=1;
    }
    return commit_record(dir,name);
}
static int commit_record(int dir,const char*name) {
    /* Validate rows against the proposed whole saved context, atomically. */
    used=0;if(object(0)||ext_settings_decode(&schema,output))return -1;
    char file[80],temp[96];snprintf(file,sizeof(file),"%s.json",name);
    snprintf(temp,sizeof(temp),".%s.%ld.new",name,(long)getpid());
    int f=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(f<0)return -1;
    size_t done=0;ssize_t w;
    while(done<used&&(w=write(f,output+done,used-done))>0)done+=(size_t)w;
    int ok=done==used&&!fsync(f);close(f);
    if(ok)ok=!settings_lock_current(dir,active_settings_lock)&&!renameat(dir,temp,dir,file);
    if(ok)ok=!fsync(dir);
    unlinkat(dir,temp,0);return ok?0:-1;
}
int main(int argc,char**argv) {
    if(argc==2&&!strcmp(argv[1],"check")) {
        size_t n=fread(schema.json,1,sizeof(schema.json),stdin);
        if(n>EXT_CAP||memchr(schema.json,0,n))return 2;
        schema.json[n]=0;return ext_parse(&schema)==1?0:2;
    }
    if(argc<3)return 2;
    const char *cmd=argv[1],*script=argv[2],*name=strrchr(script,'/');name=name?name+1:script;
    if(!settings_name(name))return 2;
    int bound=!strcmp(cmd,"run-script");
    int on_upload=!strcmp(cmd,"greeting-upload");
    int greeting=on_upload||!strcmp(cmd,"greeting"), managed=0, greeting_saved=0;
    char installed[sizeof LX06_GREETING_APPS + 65];
    snprintf(installed,sizeof installed,LX06_GREETING_APPS "/%s",name);
    managed=geteuid()==0&&!strcmp(installed,script);
    if(greeting&&(!managed||argc!=3))return 2;
    int restoring=!strcmp(cmd,"restore"),saving=!strcmp(cmd,"save")||restoring,showing=!strcmp(cmd,"show"),running=!strcmp(cmd,"run")||bound,forgetting=!strcmp(cmd,"forget"),hasoptions=!strcmp(cmd,"has-options"),forgetoptions=!strcmp(cmd,"forget-options");
    if((!saving&&!showing&&!running&&!forgetting&&!hasoptions&&!forgetoptions&&!greeting)||(running&&!bound&&argc<4))return 2;
    int dir=-1,lock=-1,f=-1;
    if(bound){
        if(!script_exec_arguments(argc,argv)||fcntl(SCRIPT_EXEC_FD,F_GETFD)>=0||errno!=EBADF)goto bad;
        /* Reserve before our own directory/cache opens. Caller descriptors
         * 3..8 remain untouched and cannot make an internal open steal 9. */
        f=open(script,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(f<0)goto bad;
        if(f!=SCRIPT_EXEC_FD){
            if(dup2(f,SCRIPT_EXEC_FD)<0)goto bad;
            close(f);f=SCRIPT_EXEC_FD;
            if(fcntl(f,F_SETFD,FD_CLOEXEC))goto bad;
        }
    }
    /* Lock before reading declarations so a cooperating upload cannot swap
     * schema between parse and saved-record commit. Read-only launches keep
     * their existing missing-store behavior; a present store is serialized. */
    if(saving||showing||running||forgetting||greeting) {
        dir=settings_dir(saving||greeting);
        if(dir<0&&errno!=ENOENT)goto bad;
        if(saving&&dir<0)goto bad;
        if(dir>=0) {
            for (int attempt=0;;attempt++) {
                lock=settings_lock_acquire(dir);
                if(lock>=0)break;
                if(!on_upload||errno!=EBUSY||attempt==39)goto bad;
                usleep(250000);
            }
        }
        active_settings_lock=lock;
    }
    if(!bound)f=open(script,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    SETTINGS_STAT st;
    if(f<0)goto bad;
    int ok=!SETTINGS_FSTAT(f,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&!(st.st_mode&0022)&&ext_read(f,&schema)==1;
    if(bound)ok=ok&&script_exec_valid_stat(&st)&&script_exec_contract(f)&&script_exec_current(f,script,&st);
    else {close(f);f=-1;}
    if(!ok)goto bad;
    if(restoring) {
        if(argc<4)goto bad;
        for(int a=3;a<argc;a++){int i=ext_field_index(&schema,argv[a]);if(i<0)goto bad;restore_mask|=1U<<i;}
    }
    int cachedir=ext_cache_environment(),catalogue=0;
    if(forgetoptions) {
        if(cachedir<0)return errno==ENOENT?0:2;
        int rc=ext_cache_forget(cachedir);close(cachedir);return rc?2:0;
    }
    if(cachedir>=0){catalogue=ext_cache_load(&schema,cachedir);close(cachedir);}
    else if(errno!=ENOENT)catalogue=-1;
    if(hasoptions)return catalogue<0?2:(catalogue?0:1);
    if(dir>=0&&read_stored(dir,name)<0)goto bad;
    if(forgetting) {
        char file[80];snprintf(file,sizeof(file),"%s.json",name);
        if(dir>=0&&(settings_lock_current(dir,lock)||(unlinkat(dir,file,0)&&errno!=ENOENT)))goto bad;
        if(dir>=0)fsync(dir);
        if(lock>=0)close(lock);
        if(dir>=0)close(dir);
        return 0;
    }
    if(!running&&!restoring&&defaults())goto bad;
    char previous_greeting[GREETING_TEXT_BYTES];
    const char *before_greeting=saving&&managed?configured_greeting():NULL;
    int had_greeting=before_greeting&&strlen(before_greeting)<sizeof previous_greeting;
    if(had_greeting)strcpy(previous_greeting,before_greeting);
    if(restoring) {
        /* Targeted override restore uses the existing transaction and keeps
         * unrelated values. Explicit field names, no implicit whole reset. */
        if(argc<4)goto bad;
        for(int a=3;a<argc;a++) {int i=ext_field_index(&schema,argv[a]);if(i<0)goto bad;present[i]=0;values[i][0]=0;}
        if(commit_record(dir,name)||defaults())goto bad;
    }
    if(saving&&!restoring&&save(dir,name))goto bad;
    if(lock>=0&&settings_lock_current(dir,lock))goto bad;
    if(greeting||(managed&&saving)) {
        const char *text=configured_greeting();
        int changed=text&&(!had_greeting||strcmp(previous_greeting,text));
        if(greeting||changed)greeting_saved=refresh_greeting(script,name,text,&st,dir,&lock)?-1:1;
        if(greeting) {
            if(!text)return message("此脚本没有问候语。",0);
            return message(greeting_saved<0?"问候音频生成失败，请重试。":
                           *text?"问候音频已生成。":"已关闭问候语。",greeting_saved<0?1:0);
        }
    }
    if(!bound){
        if(lock>=0)close(lock);
        lock=-1;
        active_settings_lock=-1;
        if(dir>=0)close(dir);
        dir=-1;
    }
    if(bound) {
        for(int i=0;i<schema.nf;i++)if(present[i]) {
            char env[65];if(!ext_text(&schema.doc,schema.fields[i].token,"env",env,sizeof(env),0))goto bad;
            if(*env&&setenv(env,values[i],1))goto bad;
        }
        /* Named admission happens while the original settings lock is
         * held. The subsequent exec uses only the retained descriptor. */
        if(!script_exec_environment(script,name)||!script_exec_current(f,script,&st)||
           (lock>=0&&settings_lock_current(dir,lock))||fcntl(f,F_SETFD,0))goto bad;
        if(lock>=0)close(lock);
        lock=-1;active_settings_lock=-1;
        if(dir>=0)close(dir);
        dir=-1;
        char *arguments[SCRIPT_EXEC_MAX_ARGS+3];
        arguments[0]="sh";arguments[1]="/proc/self/fd/9";
        for(int i=3;i<argc;i++)arguments[i-1]=argv[i];
        arguments[argc-1]=NULL;
        execv("/bin/sh",arguments);close(f);return 127;
    }
    if(running) {
        for(int i=0;i<schema.nf;i++)if(present[i]) {
            char env[65];if(!ext_text(&schema.doc,schema.fields[i].token,"env",env,sizeof(env),0))return 2;
            if(*env&&setenv(env,values[i],1))return 2;
        }
        if(managed&&setenv("LX06_SCRIPT_ID",name,1))return 2;
        execvp(argv[3],argv+3);return 127;
    }
    used=0;
    const char *notice=greeting_saved<0?"设置已保存，问候音频生成失败，请重试。":
        greeting_saved>0?"设置已保存，问候音频已更新。":
        retained_unknown?"存在当前脚本未声明的保留字段，请检查配置兼容性；原值已保留。":
        saving?"已保存；对话参数下次会话生效，小爱通道词约 2 秒后生效。密钥不回显，留空保留原值。":"已读取当前配置；密钥不回显，留空保留原值。";
    if(append("{\"message\":")||quoted(notice)||append(",\"values\":")||object(1))return message("配置输出超过限制",2);
    if(schema.options_json[0]) {
        char options[EXT_EVENT_CAP+1];
        size_t before=used;
        if(ext_cache_options(&schema,options)<0)return message("模型目录格式错误",2);
        /* Large catalogues still travel separately in authenticated state.
         * Never lose successfully read settings just to fit both in one line. */
        if(append(",\"options\":")||append(options)||used+2>=sizeof(output)){used=before;output[used]=0;}
    }
    if(append("}")||!ext_public_schema(output,&schema))return message("配置输出超过限制",2);
    puts(output);return 0;
bad:
    if(f>=0)close(f);
    if(lock>=0)close(lock);
    active_settings_lock=-1;
    if(dir>=0)close(dir);
    if(running){fputs("script-settings: invalid or unavailable configuration; not starting session\n",stderr);return 2;}
    return message("配置读取或保存失败：请检查参数范围、配置权限及剩余空间；原文件未被参数校验失败覆盖。",2);
}
