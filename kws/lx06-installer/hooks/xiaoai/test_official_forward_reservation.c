#define _GNU_SOURCE
#include "official-forward-store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s (errno=%d)\n",__FILE__,__LINE__,#x,errno); exit(1); } } while (0)
static int lookup(void *p,const char *s) { (void)p; return !strcmp(s,"demo.sh"); }
static int admit(void *p,const char *s) { int *n=p; ++*n; return !strcmp(s,"official"); }
static int reject(void *p,const char *s) { (void)p; (void)s; return 0; }
struct gate { int go, started, finished; };
static int gated_admit(void *p,const char *s) {
    struct gate *g=p; char c='x'; struct pollfd fd={g->finished,POLLIN,0};
    CHECK(!strcmp(s,"official")); CHECK(write(g->go,&c,1)==1);
    CHECK(read(g->started,&c,1)==1);
    CHECK(poll(&fd,1,250)==0); /* select must not finish inside admission */
    return 1;
}
int main(void) {
    char dir[1024]; const char *tmp=getenv("TMPDIR"); int n=0, status;
    CHECK(tmp && snprintf(dir,sizeof dir,"%s/of-reserve-XXXXXX",tmp)>0);
    CHECK(mkdtemp(dir)); int d=open(dir,O_RDONLY|O_DIRECTORY); CHECK(d>=0);
    struct of_store_snapshot snap;
    CHECK(of_store_snapshot(d,&snap)==0);
    CHECK(!strcmp(snap.selection,"official"));
    CHECK(of_store_reserve(d,&snap,admit,&n)==0 && n==1);
    pid_t child=fork(); CHECK(child>=0);
    if (!child) _exit(of_store_select(d,"demo.sh",lookup,NULL)!=0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    errno=0; CHECK(of_store_reserve(d,&snap,admit,&n)<0 && errno==ESTALE && n==1);
    CHECK(of_store_select(d,"official",NULL,NULL)==0);
    errno=0; CHECK(of_store_reserve(d,&snap,admit,&n)<0 && errno==ESTALE && n==1);
    of_store_snapshot_close(&snap);
    CHECK(of_store_snapshot(d,&snap)==0);
    errno=0; CHECK(of_store_reserve(d,&snap,reject,NULL)<0 && errno==ECANCELED);
    CHECK(!fchmod(d,0777));
    errno=0; CHECK(of_store_reserve(d,&snap,admit,&n)<0 && n==1);
    CHECK(!fchmod(d,0700));
    int go[2],started[2],finished[2]; CHECK(!pipe(go) && !pipe(started) && !pipe(finished));
    child=fork(); CHECK(child>=0);
    if(!child) {
        char c; close(go[1]); close(started[0]); close(finished[0]);
        if(read(go[0],&c,1)!=1 || write(started[1],&c,1)!=1 ||
           of_store_select(d,"demo.sh",lookup,NULL) || write(finished[1],&c,1)!=1) _exit(1);
        _exit(0);
    }
    close(go[0]); close(started[1]); close(finished[1]);
    struct gate g={go[1],started[0],finished[0]};
    CHECK(of_store_reserve(d,&snap,gated_admit,&g)==0);
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    close(go[1]); close(started[0]); close(finished[0]);
    CHECK(of_store_select(d,"official",NULL,NULL)==0);
    errno=0; CHECK(of_store_reserve(d,&snap,admit,&n)<0 && errno==ESTALE && n==1);
    of_store_snapshot_close(&snap);
    CHECK(!unlinkat(d,"official-forward",0)); CHECK(!close(d)); CHECK(!rmdir(dir));
    puts("official forwarding reservation: PASS"); return 0;
}
