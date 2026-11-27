#define _GNU_SOURCE
#include "led-fixture.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned assertions;
#define CHECK(expr) do{assertions++;if(!(expr)){fprintf(stderr,"owner-fixture-failed line=%u\n",__LINE__);exit(1);}}while(0)
static long long tick_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static char silent_file[256];
#define LX06_SILENT_FILE silent_file
static char lease_dir[256];
#define LX06_LIGHT_LEASE_DIR lease_dir
#include "../../../../hooks/xiaoai/dialog-light-owner.h"
static void path(char*out,const char*name){snprintf(out,256,"%s/%s",getenv("DC_LED_DIRECTORY"),name);}
static void marker(const char*name,int on){char file[256];path(file,name);if(on){int f=open(file,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(f>=0);close(f);}else CHECK(!unlink(file));}
static void step(struct light_owner*l,int lock){int status;pid_t pid;while((pid=waitpid(-1,&status,WNOHANG))>0)light_reaped(l,pid,status);light_tick(l,lock,tick_ms());struct timespec wait={0,1000000};nanosleep(&wait,NULL);}
static void settle(struct light_owner*l,int lock,int state){long long end=tick_ms()+2500;do{step(l,lock);}while((l->worker||l->applied!=state)&&tick_ms()<end);CHECK(!l->worker&&l->applied==state);}
static void send_state(int fd,int state){unsigned char b=(unsigned char)state;CHECK(send(fd,&b,1,0)==1);}
static struct led_fixture_record ledger(void){char file[256];path(file,"ledger");int fd=open(file,O_RDONLY);CHECK(fd>=0);struct led_fixture_record r;CHECK(read(fd,&r,sizeof(r))==sizeof(r));close(fd);return r;}
static void replace_effects(unsigned effects){char file[256];path(file,"ledger");int fd=open(file,O_RDWR);CHECK(fd>=0&&!flock(fd,LOCK_EX));struct led_fixture_record record;CHECK(pread(fd,&record,sizeof(record),0)==sizeof(record));record.bits=effects;CHECK(pwrite(fd,&record,sizeof(record),0)==sizeof(record));close(fd);}
int main(int argc,char**argv){CHECK(argc==2);char directory[]="/tmp/dc-led-owner-XXXXXX";CHECK(mkdtemp(directory));CHECK(!setenv("DC_LED_DIRECTORY",directory,1));CHECK(!setenv("LX06_DIALOG_LIGHTS","1",1));path(silent_file,"silent");path(lease_dir,"lease-dir");
    char file[256];path(file,"ledger");int ledgerfd=open(file,O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(ledgerfd>=0);struct led_fixture_record initial={.bits=1u<<1};CHECK(write(ledgerfd,&initial,sizeof(initial))==sizeof(initial));close(ledgerfd);marker("active",1);
    path(file,"lock");int lock=open(file,O_CREAT|O_RDWR,0600);CHECK(lock>=0);
    struct light_owner l={0};int channel=light_channel(&l);CHECK(channel>=0);settle(&l,lock,LX06_LIGHT_WAKE);
    if(!strcmp(argv[1],"transition")) {int states[]={LX06_LIGHT_THINKING,LX06_LIGHT_SPEAKING,LX06_LIGHT_INTERRUPTED,LX06_LIGHT_LISTENING,LX06_LIGHT_SPEAKING};for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);i++){send_state(channel,states[i]);settle(&l,lock,states[i]);CHECK(ledger().bits!=0);}}
    else if(!strcmp(argv[1],"same-effect")){unsigned count=ledger().operations;send_state(channel,LX06_LIGHT_LISTENING);settle(&l,lock,LX06_LIGHT_LISTENING);send_state(channel,LX06_LIGHT_INTERRUPTED);settle(&l,lock,LX06_LIGHT_INTERRUPTED);CHECK(ledger().operations==count);}
    else if(!strcmp(argv[1],"failure-keeps-light")){marker("fail",1);send_state(channel,LX06_LIGHT_SPEAKING);settle(&l,lock,LX06_LIGHT_SPEAKING);CHECK(l.failed);CHECK(ledger().bits!=0);marker("fail",0);
        long long end=tick_ms()+LX06_LIGHT_CHECK_MS+600;while(ledger().bits!=(1u<<3)&&tick_ms()<end)step(&l,lock);CHECK(ledger().bits==(1u<<3));}
    else if(!strcmp(argv[1],"restore-cleared-effect")){
        unsigned operations=ledger().operations;long long healthy=tick_ms()+LX06_LIGHT_CHECK_MS+100;
        while(tick_ms()<healthy)step(&l,lock);
        CHECK(ledger().operations==operations);
        replace_effects(0);
        long long end=tick_ms()+LX06_LIGHT_CHECK_MS+600;while(!ledger().bits&&tick_ms()<end)step(&l,lock);
        CHECK(ledger().bits==(1u<<1)&&ledger().operations==operations+1);
        replace_effects(1u<<8);
        end=tick_ms()+LX06_LIGHT_CHECK_MS+600;while(!(ledger().bits&(1u<<1))&&tick_ms()<end)step(&l,lock);
        CHECK(ledger().bits==((1u<<1)|(1u<<8))&&ledger().operations==operations+2);
        replace_effects(1u<<1);
    }
    else if(!strcmp(argv[1],"silent-defers-restore")){
        int silent=open(silent_file,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(silent>=0);CHECK(write(silent,"1",1)==1);close(silent);
        replace_effects(0);unsigned operations=ledger().operations;
        long long end=tick_ms()+LX06_LIGHT_CHECK_MS+100;while(tick_ms()<end)step(&l,lock);
        CHECK(!ledger().bits&&ledger().operations==operations);CHECK(!unlink(silent_file));
        end=tick_ms()+LX06_LIGHT_CHECK_MS+600;while(!ledger().bits&&tick_ms()<end)step(&l,lock);
        CHECK(ledger().bits==(1u<<1)&&ledger().operations==operations+1);
    }
    else if(!strcmp(argv[1],"native-cleanup-interlock")){
        /* The native wrapper uses an independent open description. */
        int native=light_lease();CHECK(native<0);
        send_state(channel,LX06_LIGHT_SPEAKING);settle(&l,lock,LX06_LIGHT_SPEAKING);
        CHECK(light_lease()<0&&ledger().bits==(1u<<3));
    }
    else if(!strcmp(argv[1],"client-end-active")){send_state(channel,LX06_LIGHT_END);for(unsigned i=0;i<40;i++)step(&l,lock);CHECK(l.desired!=LX06_LIGHT_END&&ledger().bits!=0);}
    else if(!strcmp(argv[1],"slow-obsolete")||!strcmp(argv[1],"close-slow")){
        send_state(channel,LX06_LIGHT_THINKING);settle(&l,lock,LX06_LIGHT_THINKING);marker("delay",1);send_state(channel,LX06_LIGHT_SPEAKING);
        path(file,"started");long long end=tick_ms()+1000;while(access(file,F_OK)&&tick_ms()<end)step(&l,lock);CHECK(!access(file,F_OK));
        if(!strcmp(argv[1],"slow-obsolete")){send_state(channel,LX06_LIGHT_LISTENING);settle(&l,lock,LX06_LIGHT_LISTENING);struct timespec wait={0,350000000};nanosleep(&wait,NULL);CHECK(!ledger().show3);CHECK(ledger().bits==(1u<<1));}
    }else CHECK(0);
    CHECK(ledger().dark_while_active==0);marker("active",0);close(channel);light_close(&l,lock);CHECK(ledger().bits==0);close(lock);
    int native=light_lease();CHECK(native>=0);close(native);
    char lease[300];snprintf(lease,sizeof(lease),"%s/lease",lease_dir);
    CHECK(!unlink(lease)&&!rmdir(lease_dir));
    const char*files[]={"ledger","lock","delay","started"};for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);i++){path(file,files[i]);if(unlink(file))CHECK(errno==ENOENT);}CHECK(!rmdir(directory));
    printf("{\"passed\":true,\"assertions\":%u}\n",assertions);return 0;
}
