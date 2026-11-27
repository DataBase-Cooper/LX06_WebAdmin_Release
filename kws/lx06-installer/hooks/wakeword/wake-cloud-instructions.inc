/* The detector is built as a standalone translation unit, just like the
 * other wake helpers above. Use the stock router's JSON token semantics. */
#include "../../runtime/doubao/src/json.c"
struct p05_instruction_cursor {
    struct stat state;
    int exists;
};
static struct p05_instruction_cursor p05_last_aux;
static const char *p05_instruction_path(void) {
    const char *path=getenv("LX06_WAKE_INSTR_LOG");
    return path && *path ? path : WAKE_INSTRUCTION_LOG;
}
static int p05_same_log(const struct stat *a,const struct stat *b) {
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size &&
           a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int p05_log_snapshot(struct p05_instruction_cursor *cursor) {
    cursor->exists=0;
    if(lstat(p05_instruction_path(),&cursor->state)) return errno==ENOENT;
    cursor->exists=1;
    return S_ISREG(cursor->state.st_mode);
}
static int p05_aux_instruction(const char *line,size_t size,const char *owned) {
    xz_json_doc doc;
    if(xz_json_parse(&doc,line,size)) return 0;
    int header=xz_json_object_get(&doc,doc.root,"header");
    char id[96];
    if(xz_json_get_string(&doc,header,"dialog_id",id,sizeof(id)) || strcmp(id,owned)) return 0;
    return 1;
}
/* Any truncation, replacement, incomplete line, oversized delta or other
 * dialog stays busy. Only complete entries for the helper's exact ready ID
 * can advance the trusted cursor. No speech or dialog ID is logged. */
static int p05_check_instructions(struct p05_instruction_cursor *cursor,const char *owned) {
    struct p05_instruction_cursor next;
    if(!p05_log_snapshot(&next)) return 0;
    if(!next.exists) return !cursor->exists;
    if(cursor->exists && p05_same_log(&next.state,&cursor->state)) return 1;
    if(!*owned || (cursor->exists && (next.state.st_ino!=cursor->state.st_ino ||
       next.state.st_dev!=cursor->state.st_dev))) return 0;
    off_t off=cursor->exists ? cursor->state.st_size : 0;
    off_t len=next.state.st_size-off;
    if(len<=0 || len>16384) return 0;
    int fd=open(p05_instruction_path(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0) return 0;
    char buf[16384];
    struct stat st;
    int ok=pread(fd,buf,(size_t)len,off)==len && !fstat(fd,&st) && p05_same_log(&st,&next.state);
    close(fd);
    for(size_t start=0;ok && start<(size_t)len;) {
        char *end=memchr(buf+start,'\n',(size_t)len-start);
        if(!end || !p05_aux_instruction(buf+start,(size_t)(end-buf)-start,owned)) return 0;
        start=(size_t)(end-buf)+1;
    }
    if(ok) { *cursor=next;p05_last_aux=next; }
    return ok;
}
