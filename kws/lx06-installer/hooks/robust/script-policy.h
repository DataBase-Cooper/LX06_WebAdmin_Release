#ifndef LX06_SCRIPT_POLICY_H
#define LX06_SCRIPT_POLICY_H
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef LX06_POLICY_STAT
#define LX06_POLICY_STAT struct stat
#define LX06_POLICY_FSTAT fstat
#endif
/* One gate for boot, new jobs and wake mailboxes. Missing state retains the
 * established default-on behavior; malformed/unreadable state fails closed.
 * Open directory and file without following their final symlinks; never block
 * on an unexpected FIFO. Reads do not create DATA content. */
#ifndef LX06_SCRIPT_POLICY_DIR
#define LX06_SCRIPT_POLICY_DIR "/data/lx06"
#endif
static int lx06_scripts_allowed(void) {
    char b[16]; ssize_t n;
    int d=open(LX06_SCRIPT_POLICY_DIR,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(d<0)return errno==ENOENT;
    LX06_POLICY_STAT st;
    if(LX06_POLICY_FSTAT(d,&st)||st.st_uid!=0||(st.st_mode&0022)){close(d);return 0;}
    int f=openat(d,"autostart",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    int e=errno;close(d);errno=e;
    if(f<0)return errno==ENOENT;
    if(LX06_POLICY_FSTAT(f,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||(st.st_mode&0022)){close(f);return 0;}
    do {n=read(f,b,sizeof(b));} while(n<0&&errno==EINTR);
    close(f);
    if(n<0||n==(ssize_t)sizeof(b))return 0;
    while(n>0&&b[n-1]=='\n')n--;
    return n==2&&b[0]=='o'&&b[1]=='n';
}
#endif
