/* HOST ONLY: original readers/snapshots and minimally adapted writer. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "unbind is fixture-only"
#endif
#define LX06_STATBUF struct stat
#define LX06_FSTAT fstat
static int app_regular(int fd) {
  LX06_STATBUF st;
  if (LX06_FSTAT(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
      st.st_nlink != 1 || (st.st_mode & 0022)) { errno = EPERM; return -1; }
  return 0;
}
static int app_open_file(int dir, const char *name, int flags) {
  int fd = openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -1;
  if (app_regular(fd) != 0) { close(fd); return -1; }
  return fd;
}
#undef LX06_STATBUF
#undef LX06_FSTAT
struct wc_blob { struct stat stat; size_t size; unsigned char *bytes; int present; };
static void wc_blob_release(struct wc_blob *b) {
    if(b->bytes) {
        volatile unsigned char *p=b->bytes;
        for(size_t i=0;i<b->size;i++) p[i]=0;
        free(b->bytes);
    }
    memset(b,0,sizeof(*b));
}
static int wc_blob_capture(int dir,const char *name,size_t cap,struct wc_blob *b) {
    int fd=app_open_file(dir,name,O_RDONLY);
    if(fd<0) return errno==ENOENT?0:-1;
    struct stat after; int rc=-1;
    if(fstat(fd,&b->stat)||b->stat.st_size<0||(uintmax_t)b->stat.st_size>cap) goto done;
    b->size=(size_t)b->stat.st_size;
    b->bytes=calloc(b->size+1,1);
    if(!b->bytes||ctl_pread_exact(fd,(char *)b->bytes,b->size,0)||
       fstat(fd,&after)||!ctl_same_stat(&b->stat,&after)) goto done;
    b->present=1;rc=0;
done:
    close(fd);return rc;
}
static int wc_blob_current(int dir,const char *name,const struct wc_blob *b) {
    int fd=app_open_file(dir,name,O_RDONLY);
    if(fd<0) return !b->present&&errno==ENOENT?0:-1;
    int rc=-1;struct stat st,after;unsigned char block[1024];
    if(!b->present||fstat(fd,&st)||!ctl_same_stat(&st,&b->stat)) goto done;
    for(size_t i=0;i<b->size;) {
        size_t n=b->size-i;if(n>sizeof(block)) n=sizeof(block);
        if(ctl_pread_exact(fd,(char *)block,n,(off_t)i)||memcmp(block,b->bytes+i,n)) goto done;
        i+=n;
    }
    if(fstat(fd,&after)||!ctl_same_stat(&st,&after)) goto done;
    rc=0;
done:
    close(fd);return rc;
}
static int ru_guarded_rename(const char *,const char *);
static int ru_temp_opened(int,const char *);
static int ru_temp_unlink(const char *);
static inline int ru_original_write_at(const char *path, const char *id) {
    char tmp[256];
    int fd, ok;
    if (id && *id && !lx06_bind_name_ok(id)) return -1;
    if (strlen(path) + 5 >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (ru_temp_opened(fd,tmp)) { close(fd); return -1; }
    ok = 1;
    if (id && *id) {
        char line[80];
        int n = snprintf(line, sizeof(line), "%s\n", id);
        ok = n > 0 && write(fd, line, (size_t)n) == n;
    }
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || ru_guarded_rename(tmp, path) != 0) { ru_temp_unlink(tmp); return -1; }
    return 0;
}

