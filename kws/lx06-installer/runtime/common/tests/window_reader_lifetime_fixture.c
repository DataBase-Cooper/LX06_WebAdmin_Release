#define _GNU_SOURCE
#include "../control_asr.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {int fd,report;pid_t pid;char path[108];} peer;
static int failures;
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"fixture failure line=%d errno=%d\n",__LINE__,errno);exit(2);}}while(0)
static void count(const char *stage,int expected){
    int actual=lx06_control_asr_window_readers();
    printf("stage=%s readers=%d expected=%d\n",stage,actual,expected);
    if(actual!=expected)failures++;
}
static peer start_peer(const char *directory,unsigned i){
    peer p={.fd=-1,.report=-1};
    REQUIRE(snprintf(p.path,sizeof(p.path),"%s/peer-%u",directory,i)>0);
    int server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0),pipefd[2];REQUIRE(server>=0&&!pipe(pipefd));
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,p.path);
    REQUIRE(!bind(server,(struct sockaddr *)&address,sizeof(address))&&!listen(server,1));
    p.pid=fork();REQUIRE(p.pid>=0);
    if(!p.pid){
        close(pipefd[0]);int fd=accept(server,NULL,NULL);if(fd<0)_exit(2);close(server);
        unsigned report[3]={0};unsigned char data[8192];ssize_t n;
        while((n=recv(fd,data,sizeof(data),0))>0){
            if((data[0]=='V'||data[0]=='S')&&n==5)report[data[0]=='V'?0:1]++;
            else if(data[0]=='A'&&n>3&&((unsigned)data[1]*256+data[2])==(unsigned)n-3)report[2]++;
            else _exit(3);
        }
        if(n<0||write(pipefd[1],report,sizeof(report))!=sizeof(report))_exit(4);
        close(fd);close(pipefd[1]);_exit(0);
    }
    close(server);close(pipefd[1]);p.report=pipefd[0];return p;
}
static void finish_peer(peer *p,unsigned profile,unsigned audio){
    unsigned report[3]={0};REQUIRE(read(p->report,report,sizeof(report))==sizeof(report));close(p->report);
    int status;REQUIRE(waitpid(p->pid,&status,0)==p->pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
    REQUIRE(report[profile==LX06_ASR_QUESTION?1:0]==1&&report[2]==audio);
    printf("peer_reaped=1 profile=%u actual_audio_packets=%u\n",profile,report[2]);REQUIRE(!unlink(p->path));
}
static void feed(lx06_control_asr *a,unsigned now){short pcm[320]={0};REQUIRE(!lx06_control_asr_feed(a,1,pcm,sizeof(pcm),now));}
int main(int argc,char **argv){
    REQUIRE(argc==2);count("initial",0);
    peer first=start_peer(argv[1],0),second=start_peer(argv[1],1);
    lx06_control_asr *a=lx06_control_asr_open_profile(first.path,LX06_ASR_WINDOW);
    lx06_control_asr *b=lx06_control_asr_open_profile(second.path,LX06_ASR_WINDOW);REQUIRE(a&&b);
    count("two_live",2);feed(a,1000);feed(b,1000);
    char missing[108];REQUIRE(snprintf(missing,sizeof(missing),"%s/missing",argv[1])>0);
    REQUIRE(!lx06_control_asr_open_profile(missing,LX06_ASR_WINDOW));count("failed_open_keeps_two",2);
    feed(a,1020);feed(b,1020);lx06_control_asr_close(a);count("close_first_keeps_second",1);finish_peer(&first,LX06_ASR_WINDOW,2);
    REQUIRE(!lx06_control_asr_open_profile(missing,LX06_ASR_WINDOW));count("failed_open_keeps_one",1);feed(b,1040);
    REQUIRE(!lx06_control_asr_open_profile(missing,99));count("invalid_profile_keeps_one",1);
    lx06_control_asr_close(b);count("both_closed",0);finish_peer(&second,LX06_ASR_WINDOW,3);
    peer legacy=start_peer(argv[1],2);lx06_control_asr *c=lx06_control_asr_open_profile(legacy.path,LX06_ASR_CONTROL);REQUIRE(c);
    count("legacy_not_window",0);feed(c,1000);lx06_control_asr_close(c);finish_peer(&legacy,LX06_ASR_CONTROL,1);count("final",0);
    printf("result=%s actual_server_processes=3 real_opus_audio_packets=6 global_idle_authority=0\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
