/* Offline production player: backend failure and post-drain poll cadence. */
#define _GNU_SOURCE
#include "../io/io.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
static atomic_int opens,drops,polls,fail_poll,clobber_errno;
int __real_poll(struct pollfd *,nfds_t,int);
int __wrap_poll(struct pollfd *p,nfds_t n,int t){atomic_fetch_add(&polls,1);if(atomic_exchange(&fail_poll,0)){errno=EBADF;return -1;}return __real_poll(p,n,t);}
static int op(void*x,unsigned r){(void)x;(void)r;atomic_fetch_add(&opens,1);return 0;}
static int pf(void*x,struct pollfd*p,unsigned n){(void)x;(void)p;(void)n;return 0;}
static ssize_t wr(void*x,const int16_t*p,size_t n){(void)x;(void)p;return (ssize_t)n;}
static int delay(void*x,long*n){(void)x;*n=0;return 0;}
static int prep(void*x){(void)x;return 0;}
static int drop(void*x){(void)x;atomic_fetch_add(&drops,1);return atomic_load(&clobber_errno)?0:-EIO;}
static void closeb(void*x){(void)x;if(atomic_load(&clobber_errno))errno=ENOSPC;}
static void check(int ok,const char *message){if(!ok){fprintf(stderr,"FAIL: %s\n",message);exit(1);}}
static dc_io_event wait_event(dc_io_player*p){for(int i=0;i<3000;i++){dc_io_event e;if(dc_io_player_next_event(p,&e)>0)return e;usleep(1000);}fprintf(stderr,"event timeout\n");exit(2);}
int main(int argc,char **argv){
 check(argc==2,"case argument");dc_io_playback_backend b={NULL,op,pf,wr,delay,prep,drop,closeb};dc_tag a={1,1,1};dc_io_player*p=dc_io_player_create(&b);check(p!=NULL,"create");
 check(!dc_io_player_begin(p,a,16000),"begin");
 if(!strcmp(argv[1],"drop")){
  for(int i=0;i<3000&&!atomic_load(&opens);i++)usleep(1000);check(atomic_load(&opens),"opened");check(!dc_io_player_stop(p,a),"stop");dc_io_event e=wait_event(p);
  fprintf(stderr,"drop calls=%d event=%d error=%d\n",atomic_load(&drops),e.type,e.error);
  check(atomic_load(&drops)==1,"drop called");check(e.type==DC_IO_STOP_FAILED&&e.error==EIO,"drop error must not acknowledge silence");
 }else if(!strcmp(argv[1],"errno")){
  for(int i=0;i<3000&&!atomic_load(&opens);i++)usleep(1000);check(atomic_load(&opens),"opened");
  atomic_store(&clobber_errno,1);atomic_store(&fail_poll,1);
  dc_io_event e=wait_event(p);fprintf(stderr,"poll error=%d drops=%d\n",e.error,atomic_load(&drops));
  check(e.type==DC_IO_ERROR&&e.error==EBADF,"poll errno survives backend cleanup");
 }else {check(!dc_io_player_finish(p,a),"finish");dc_io_event e=wait_event(p);check(e.type==DC_IO_DRAINED,"drained");int before=atomic_load(&polls);usleep(250000);int n=atomic_load(&polls)-before;fprintf(stderr,"polls after drain in 250ms=%d\n",n);check(n<=8,"drained worker must not poll every 5ms");}
 dc_io_player_destroy(p);return 0;
}
