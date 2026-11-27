/* Opt-in shell-fd-v1 source identity for original rmio -> keeper -> runner.
 * Inode/stat binding only, never a sealed byte snapshot or an ABA guarantee.
 * Include after script-settings-store.h to use the caller's SETTINGS_STAT ABI.
 * The independent run-script helper retains its caller-owned/empty-FD9 rules. */
#ifndef LX06_SCRIPT_EXEC_SOURCE_V1_H
#define LX06_SCRIPT_EXEC_SOURCE_V1_H
#define EXT_EXEC_SOURCE_FD 9
#define EXT_EXEC_SOURCE_CAP 512
#define EXT_EXEC_SOURCE_MAX (1024u*1024u)
/* Fixed order, unsigned decimal metadata and signed decimal size/time, no
 * parser coercion. Runner compares the complete canonical string generated
 * from its actual FD; missing, extra, padded or truncated fields cannot pass. */
static int ext_exec_source_identity(const SETTINGS_STAT *s,char out[EXT_EXEC_SOURCE_CAP]) {
    int n=snprintf(out,EXT_EXEC_SOURCE_CAP,
        "shell-fd-v1:%llu:%llu:%llu:%llu:%llu:%llu:%lld:%lld:%lld:%lld:%lld",
        (unsigned long long)s->st_dev,(unsigned long long)s->st_ino,
        (unsigned long long)s->st_mode,(unsigned long long)s->st_uid,
        (unsigned long long)s->st_gid,(unsigned long long)s->st_nlink,
        (long long)s->st_size,(long long)s->st_mtim.tv_sec,(long long)s->st_mtim.tv_nsec,
        (long long)s->st_ctim.tv_sec,(long long)s->st_ctim.tv_nsec);
    return n>0&&n<EXT_EXEC_SOURCE_CAP;
}
/* Original deployed scripts are root-owned 0755, readable by the isolated
 * keeper UID. Do not transfer this runner relation to run-script's different
 * caller-owned contract. No set-id or group/world-writable opt-in source. */
static int ext_exec_source_valid(const SETTINGS_STAT *s) {
    return S_ISREG(s->st_mode)&&(s->st_uid==0||s->st_uid==getuid())&&
        s->st_nlink==1&&!(s->st_mode&06022)&&s->st_size>0&&
        (uint64_t)s->st_size<=EXT_EXEC_SOURCE_MAX;
}
static int ext_exec_source_matches(int fd,const char *expected) {
    SETTINGS_STAT held;char identity[EXT_EXEC_SOURCE_CAP];
    return expected&&strnlen(expected,EXT_EXEC_SOURCE_CAP)<EXT_EXEC_SOURCE_CAP&&
        !SETTINGS_FSTAT(fd,&held)&&ext_exec_source_valid(&held)&&
        ext_exec_source_identity(&held,identity)&&!strcmp(identity,expected);
}
static int ext_exec_source_current(int fd,const char *path,const char *expected) {
    int named=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    int ok=named>=0&&ext_exec_source_matches(fd,expected)&&ext_exec_source_matches(named,expected);
    if(named>=0)close(named);return ok;
}
/* 0: no leading execution declaration (legacy); 1: exact existing contract;
 * -1: unreadable source or malformed/unsupported declaration. No legacy-fallback
 * after a refused declaration. The bounded leading block owns the marker. */
static int ext_exec_source_contract(int fd) {
    char bytes[16385];ssize_t n;
    do {n=pread(fd,bytes,sizeof(bytes)-1,0);} while(n<0&&errno==EINTR);
    if(n<=0)return -1;bytes[n]=0;
    int nul=memchr(bytes,0,(size_t)n)!=NULL,marker=0,shell=0,first=1,valid=1;
    char *line=bytes;
    for(;;) {
        char *end=strchr(line,'\n');if(!end)return marker?-1:0;*end=0;
        if(first){shell=!strcmp(line,"#!/bin/sh");first=0;}
        else {
            while(*line==' '||*line=='\t')line++;
            if(!strncmp(line,"# LX06-EXEC:",12)) {
                marker++;if(strcmp(line,"# LX06-EXEC: shell-fd-v1"))valid=0;
            } else if(*line&&*line!='#')
                return marker?(shell&&valid&&marker==1&&!nul&&!strcmp(line,"exec 9<&-")?1:-1):0;
        }
        line=end+1;
    }
}
static int ext_exec_source_environment(const char *path) {
    const char *name=strrchr(path,'/');name=name?name+1:path;
    return settings_name(name)&&!setenv("LX06_SCRIPT_EXEC_PROTOCOL","shell-fd-v1",1)&&
        !setenv("LX06_SCRIPT_PATH",path,1)&&!setenv("LX06_SCRIPT_ID",name,1);
}
#endif
