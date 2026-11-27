#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
 const int flags=F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE;
 int fd=syscall(SYS_memfd_create,"fixture",MFD_ALLOW_SEALING); if(fd<0){perror("memfd_create");return 1;}
 if(write(fd,"fixture",7)!=7){perror("write");return 2;}
 if(fcntl(fd,F_ADD_SEALS,flags)){perror("F_ADD_SEALS");return 2;}
 int actual=fcntl(fd,F_GET_SEALS); if(actual!=flags){fprintf(stderr,"F_GET_SEALS=%d expected=%d errno=%d\n",actual,flags,errno);return 2;}
 errno=0; if(pwrite(fd,"X",1,0)!=-1||errno!=EPERM){fprintf(stderr,"write not denied errno=%d\n",errno);return 3;}
 errno=0; if(ftruncate(fd,0)!=-1||errno!=EPERM){fprintf(stderr,"truncate not denied errno=%d\n",errno);return 4;}
 char path[64],buf[8]={0};snprintf(path,sizeof path,"/proc/self/fd/%d",fd);int copy=open(path,O_RDONLY);if(copy<0||read(copy,buf,7)!=7||strcmp(buf,"fixture")){perror("reopen");return 5;}
 printf("sealed=0x%x write/truncate denied; proc reopen exact\n",flags);close(copy);close(fd);return 0;
}
