#define _GNU_SOURCE
#include "ctl-config.h"
#include "../../../runtime/common/script-settings-store.h"
/* Read-only use of the shipped tokenizer, not another JSON implementation. */
#ifndef LX06_CTL_JSON_ALREADY_INCLUDED
#include "json.c"
#endif

_Static_assert(CFG_BYTES==SETTINGS_CAP,"retain the existing whole-record budget");

static int flat_record(const char *raw) {
    size_t n=strnlen(raw,SETTINGS_CAP+1);
    if(!n||n>SETTINGS_CAP||!cfg_utf8_valid(raw,n)) return 0;
    xz_json_doc d;
    if(xz_json_parse(&d,raw,n)) return 0;
    for(int k=d.tokens[0].first_child;k>=0;) {
        int v=d.tokens[k].next_sibling;
        char key[33],value[SETTINGS_CAP+1];
        if(v<0||decode_string(&d,k,key,sizeof(key))||!key[0]||
           d.tokens[k].end-d.tokens[k].start!=strlen(key)||
           memcmp(raw+d.tokens[k].start,key,strlen(key))||
           d.tokens[v].type!=XZ_JSON_STRING||decode_string(&d,v,value,sizeof(value))) return 0;
        /* Existing reader's strict string policy: no escaped NUL/surrogates. */
        for(size_t i=d.tokens[v].start;i<d.tokens[v].end;i++) {
            if(raw[i]!='\\') continue;
            if(raw[i+1]=='u') {
                unsigned cp;
                if(hex4(raw+i+2,&cp)||!cp||(cp>=0xd800&&cp<=0xdfff)) return 0;
                i+=5;
            } else i++;
        }
        for(int p=d.tokens[0].first_child;p!=k;p=d.tokens[d.tokens[p].next_sibling].next_sibling)
            if(d.tokens[p].end-d.tokens[p].start==strlen(key)&&
               !memcmp(raw+d.tokens[p].start,key,strlen(key))) return 0;
        k=d.tokens[v].next_sibling;
    }
    return 1;
}
int cfg_store_open(int create) { return settings_dir(create); }

int cfg_store_read(int dir,const char *script,char record[CFG_BYTES+1]) {
    SETTINGS_STAT st;
    if(dir<0||!script||!record||!settings_name(script)||SETTINGS_FSTAT(dir,&st)||
       !S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0700) return -1;
    int r=settings_read(dir,script,record);
    if(r<0) return -1;
    if(!r) { strcpy(record,"{}"); return 0; }
    char file[80]; struct stat stored; snprintf(file,sizeof(file),"%s.json",script);
    /* settings_read returns a string: also reject bytes after an embedded NUL. */
    if(fstatat(dir,file,&stored,AT_SYMLINK_NOFOLLOW)||!S_ISREG(stored.st_mode)||
       stored.st_size!=(off_t)strlen(record)||!flat_record(record)) return -1;
    return 1;
}

static int cfg_store_lock_valid(int dir,int lock) {
    SETTINGS_STAT st; struct stat named;
    return SETTINGS_FSTAT(lock,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_nlink!=1||
        (st.st_mode&0777)!=0600||fstatat(dir,".lock",&named,AT_SYMLINK_NOFOLLOW)||
        st.st_ino!=named.st_ino||st.st_dev!=named.st_dev||flock(lock,LOCK_EX|LOCK_NB)?-1:0;
}
int cfg_store_guarded_locked(int dir,int lock,const char *script,cfg_record_edit edit,
                            cfg_record_guard guard,void *ctx) {
    if(dir<0||lock<0||!script||!edit||!settings_name(script)) return -1;
    SETTINGS_STAT st;
    if(SETTINGS_FSTAT(dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0700) return -1;
    int rc=-1,fd=-1,created=0; char file[80],temp[96];
    snprintf(file,sizeof(file),"%s.json",script);
    snprintf(temp,sizeof(temp),".%s.%ld.new",script,(long)getpid());
    if(cfg_store_lock_valid(dir,lock)) goto done;
    char current[SETTINGS_CAP+1],next[SETTINGS_CAP+1];
    if(cfg_store_read(dir,script,current)<0) goto done;
    memset(next,0xff,sizeof(next));
    if(edit(current,next,ctx)||!memchr(next,0,sizeof(next))||!flat_record(next)) goto done;
    /* No-op read/reupload retains the exact existing record, no rename. */
    if(!strcmp(current,next)) { rc=cfg_store_lock_valid(dir,lock)||(guard&&guard(ctx))?-1:0; goto done; }
    fd=openat(dir,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) goto done;
    created=1;
    size_t used=strlen(next),done_bytes=0;
    while(done_bytes<used) {
        ssize_t n=write(fd,next+done_bytes,used-done_bytes);
        if(n<0&&errno==EINTR) continue;
        if(n<=0) goto done;
        done_bytes+=(size_t)n;
    }
    if(fsync(fd)) goto done;
    if(close(fd)) { fd=-1; goto done; } fd=-1;
    if(cfg_store_lock_valid(dir,lock)||(guard&&guard(ctx))) goto done;
    if(renameat(dir,temp,dir,file)) goto done;
    created=0;
    rc=fsync(dir)?1:0;
done:
    if(fd>=0) close(fd);
    /* Do not unlink somebody else's pre-existing temporary node. */
    if(created) unlinkat(dir,temp,0);
    return rc;
}
int cfg_store_transaction_locked(int dir,int lock,const char *script,cfg_record_edit edit,void *ctx) {
    return cfg_store_guarded_locked(dir,lock,script,edit,NULL,ctx);
}
int cfg_store_transaction(int dir,const char *script,cfg_record_edit edit,void *ctx) {
    SETTINGS_STAT st;
    /* Refuse an unchecked directory BEFORE creating its .lock. */
    if(dir<0||!script||!edit||!settings_name(script)||SETTINGS_FSTAT(dir,&st)||
       !S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0777)!=0700) return -1;
    int lock=openat(dir,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(lock<0) return -1;
    int rc=cfg_store_transaction_locked(dir,lock,script,edit,ctx);
    close(lock); return rc;
}
