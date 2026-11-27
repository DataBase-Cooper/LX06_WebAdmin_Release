/* Offline transport contract; this is not the production session-run seam. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../status-pipe-experiment.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s errno=%d\n",__LINE__,#x,errno); return 1; } } while(0)
int main(void) {
 struct sp_owner s={0}; struct sp_sender old, next; pid_t child; int st;
 CHECK(sp_begin(&s,&old)==0);
 child=fork(); CHECK(child>=0);
 if(!child) { sp_child_close(&s); if(sp_send(&old,SP_CAPTURE_OVERFLOW)) _exit(10); _exit(0); }
 sp_parent_close_sender(&old); CHECK(waitpid(child,&st,0)==child && WIFEXITED(st) && WEXITSTATUS(st)==0);
 CHECK(sp_drain(&s)==0); CHECK(s.latest==SP_CAPTURE_OVERFLOW); CHECK(sp_end(&s)==0);
 CHECK(s.latest==SP_CAPTURE_OVERFLOW);
 CHECK(sp_begin(&s,&next)==0); CHECK(s.latest==SP_NONE);
 CHECK(memcmp(old.token,next.token,sizeof old.token)!=0);
 old.fd=next.fd;
 CHECK(sp_send(&old,SP_CAPTURE_OVERFLOW)==0); CHECK(sp_drain(&s)==0); CHECK(s.latest==SP_NONE);
 child=fork(); CHECK(child>=0);
 if(!child) { sp_child_close(&s); if(sp_send(&next,SP_CAPTURE_OVERFLOW)) _exit(11); raise(SIGKILL); _exit(12); }
 sp_parent_close_sender(&next); CHECK(waitpid(child,&st,0)==child && WIFSIGNALED(st) && WTERMSIG(st)==SIGKILL);
 CHECK(sp_drain(&s)==0); CHECK(s.latest==SP_CAPTURE_OVERFLOW); CHECK(sp_end(&s)==0);
 CHECK(sp_begin(&s,&next)==0);
 CHECK(sp_send(&next,42)==-1 && errno==EINVAL);
 unsigned count=0; while(sp_send(&next,SP_CAPTURE_OVERFLOW)==0) { CHECK(++count<1000000); }
 CHECK(errno==EAGAIN || errno==EWOULDBLOCK); CHECK(count>0);
 CHECK(sp_drain(&s)==0); CHECK(s.latest==SP_CAPTURE_OVERFLOW);
 CHECK(sp_send(&next,SP_CAPTURE_OVERFLOW)==0); sp_parent_close_sender(&next);
 CHECK(sp_drain(&s)==0); CHECK(sp_end(&s)==0);
 printf("isolated status pipe fork/crash/stale/backpressure: PASS (%u writes before EAGAIN)\n",count); return 0;
}
