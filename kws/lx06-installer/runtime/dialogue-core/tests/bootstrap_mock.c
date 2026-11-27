/* Synthetic local broker executable: never loads an SDK or reads an account. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int sig){(void)sig;stopped=1;}
int main(int argc,char **argv){
 const char *forbidden=getenv("BOOT_FORBID_FD");if(forbidden&&fcntl(atoi(forbidden),F_GETFD)>=0)return 31;
 if(argc==2&&!strcmp(argv[1],"--helper")){signal(SIGTERM,SIG_IGN);for(;;)pause();}
 if(argc!=12||strcmp(argv[1],"--serve"))return 2;
 const char *socket_path=NULL,*worker=NULL;
 for(int i=2;i<argc;i+=2){if(!strcmp(argv[i],"--socket"))socket_path=argv[i+1];if(!strcmp(argv[i],"--worker"))worker=argv[i+1];}
 assert(socket_path&&worker);
 const char *mode=getenv("BOOT_FIXTURE_MODE");if(!mode)mode="normal";
 if(!strcmp(mode,"exit"))return 8;
 if(!strcmp(mode,"hang")){signal(SIGTERM,SIG_IGN);for(;;)pause();}
 signal(SIGTERM,!strcmp(mode,"ignore")?SIG_IGN:stop);
 if(!strcmp(mode,"fork")){
  pid_t child=fork();assert(child>=0);
  if(!child){assert(!setpgid(0,0));execl(worker,worker,"--helper",(char *)NULL);_exit(127);}
 }
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);assert(fd>=0);
 struct sockaddr_un address={0};address.sun_family=AF_UNIX;assert(strlen(socket_path)<sizeof address.sun_path);strcpy(address.sun_path,socket_path);
 assert(!bind(fd,(struct sockaddr *)&address,sizeof address)&&!chmod(socket_path,0600)&&!listen(fd,8));
 while(!stopped){struct pollfd p={fd,POLLIN,0};int ready=poll(&p,1,10);if(ready>0){int peer=accept4(fd,NULL,NULL,SOCK_CLOEXEC);if(peer>=0)close(peer);}}
 close(fd);return 0; /* Preserve socket inode for the owner's exact cleanup. */
}
