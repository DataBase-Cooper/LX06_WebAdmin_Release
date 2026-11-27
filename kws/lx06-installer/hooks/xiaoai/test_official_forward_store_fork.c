#define _GNU_SOURCE
#include "official-forward-store.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s (errno=%d)\n",__FILE__,__LINE__,#x,errno); exit(1); } } while (0)
struct fork_context { int hold[2]; pid_t child; int result; };
static int fork_callback(void *v,const char *selection) {
    struct fork_context *c=v; char byte;
    (void)selection;
    c->child=fork(); CHECK(c->child>=0);
    if (!c->child) { close(c->hold[1]); (void)read(c->hold[0],&byte,1); _exit(0); }
    close(c->hold[0]); return c->result;
}
static void run_case(int d,int reserve,int accept) {
    struct fork_context c={{-1,-1},-1,accept}; struct of_store_snapshot s;
    int done[2],started[2],status,r; char byte;
    CHECK(!of_store_select(d,"official",NULL,NULL));
    CHECK(!of_store_snapshot(d,&s)); CHECK(!pipe(c.hold));
    if (reserve) r=of_store_reserve(d,&s,fork_callback,&c);
    else r=of_store_select(d,"demo.sh",fork_callback,&c);
    CHECK((r==0)==accept); CHECK(c.child>0);
    CHECK(!pipe(done)); CHECK(!pipe(started)); pid_t writer=fork(); CHECK(writer>=0);
    if (!writer) { close(done[0]); close(started[0]); if(write(started[1],"x",1)!=1) _exit(4); if (of_store_select(d,"official",NULL,NULL)) _exit(2); if(write(done[1],"x",1)!=1) _exit(3); _exit(0); }
    close(done[1]); close(started[1]); CHECK(read(started[0],&byte,1)==1); close(started[0]);
    struct pollfd p={done[0],POLLIN,0};
    int ready=poll(&p,1,1000);
    /* Always release and reap both children, including on the expected RED. */
    CHECK(write(c.hold[1],"x",1)==1); close(c.hold[1]); CHECK(waitpid(c.child,&status,0)==c.child && WIFEXITED(status));
    if (ready!=1 || !(p.revents&POLLIN)) { kill(writer,SIGKILL); waitpid(writer,&status,0); fprintf(stderr,"FAIL inherited lock: reserve=%d accept=%d ready=%d\n",reserve,accept,ready); exit(1); }
    CHECK(read(done[0],&byte,1)==1); close(done[0]);
    CHECK(waitpid(writer,&status,0)==writer && WIFEXITED(status) && WEXITSTATUS(status)==0);
    of_store_snapshot_close(&s);
}
int main(void) {
    char path[1024]; const char *tmp=getenv("TMPDIR");
    CHECK(tmp && snprintf(path,sizeof path,"%s/of-fork-XXXXXX",tmp)>0); CHECK(mkdtemp(path));
    int d=open(path,O_RDONLY|O_DIRECTORY); CHECK(d>=0);
    run_case(d,1,1); run_case(d,1,0); run_case(d,0,1); run_case(d,0,0);
    CHECK(!unlinkat(d,"official-forward",0)); CHECK(!close(d)); CHECK(!rmdir(path));
    puts("official forwarding inherited flock: PASS"); return 0;
}
