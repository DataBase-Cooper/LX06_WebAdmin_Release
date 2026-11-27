#define _GNU_SOURCE
#include "system_file.h"
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Preserve the same stock ARM2.25 stat ABI discipline as inbox_store.c. */
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
typedef struct stat64 resource_stat;
extern int __fxstat64(int,int,struct stat64 *);
#define resource_fstat(fd,st) __fxstat64(3,(fd),(st))
#else
typedef struct stat resource_stat;
#define resource_fstat fstat
#endif

int br_system_path_valid(const char *path){
    if(!path)return 0;
    size_t n=strnlen(path,BR_SYSTEM_PATH_MAX+1),start=0;
    if(!n||n>BR_SYSTEM_PATH_MAX)return 0;
    for(size_t i=0;i<=n;i++){
        unsigned c=(unsigned char)path[i];
        if(i<n&&(c<32||c==127||c=='\\'||c==':'))return 0;
        if(i==n||c=='/'){
            size_t size=i-start;
            if(!size||(size==1&&path[start]=='.')||
               (size==2&&path[start]=='.'&&path[start+1]=='.'))return 0;
            start=i+1;
        }
    }
    return 1;
}
static int safe_fd(int fd,int directory){
    resource_stat st;
    return !resource_fstat(fd,&st)&&st.st_uid==geteuid()&&!(st.st_mode&0022)&&
        (directory?S_ISDIR(st.st_mode):S_ISREG(st.st_mode));
}
int br_system_file_open(int root,const char *path,const char *const *allowed,size_t count){
    if(!br_system_path_valid(path)||!allowed||!count||count>32||!safe_fd(root,1))return -1;
    int found=0;
    for(size_t i=0;i<count;i++){
        if(!br_system_path_valid(allowed[i]))return -1;
        for(size_t j=0;j<i;j++)if(!strcmp(allowed[j],allowed[i]))return -1;
        if(!strcmp(path,allowed[i]))found=1;
    }
    if(!found)return -1;
    int parent=fcntl(root,F_DUPFD_CLOEXEC,3);
    if(parent<0)return -1;
    char component[BR_SYSTEM_PATH_MAX+1];
    size_t start=0,n=strlen(path);
    for(size_t i=0;i<=n;i++)if(i==n||path[i]=='/'){
        size_t length=i-start;
        memcpy(component,path+start,length);component[length]=0;
        int flags=O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC;
        if(i<n)flags|=O_DIRECTORY;
        int next=openat(parent,component,flags);
        close(parent);parent=-1;
        if(next<0)return -1;
        if(!safe_fd(next,i<n)){close(next);return -1;}
        parent=next;start=i+1;
    }
    return parent;
}
