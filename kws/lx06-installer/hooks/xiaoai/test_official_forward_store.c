#define _GNU_SOURCE
/* Unlike assert, CHECK evaluates its expression even with NDEBUG. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "official-forward-store.h"

static int lookup(void *p,const char *id) { return *(int*)p && !strcmp(id,"demo.sh"); }
static void put(int d,const char *s,mode_t mode) {
    int f=openat(d,"official-forward",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,mode);
    CHECK(f>=0); CHECK(write(f,s,strlen(s))==(ssize_t)strlen(s)); CHECK(!close(f));
}
int main(void) {
    char dir[1024],out[65]; int enabled=1;
    const char *tmp=getenv("TMPDIR"); CHECK(tmp && *tmp);
    CHECK(snprintf(dir,sizeof dir,"%s/of-store-XXXXXX",tmp)>0);
    CHECK(mkdtemp(dir)); int d=open(dir,O_DIRECTORY|O_RDONLY); CHECK(d>=0);
    CHECK(of_store_read(d,out)==0 && !strcmp(out,"official"));
    CHECK(of_store_select(d,"demo.sh",lookup,&enabled)==0);
    CHECK(of_store_read(d,out)==0 && !strcmp(out,"demo.sh"));
    CHECK(of_store_resolve(d,lookup,&enabled,out)==OF_SCRIPT);
    CHECK(of_store_resolve(d,NULL,NULL,out)==OF_REJECT);
    enabled=0; CHECK(of_store_resolve(d,lookup,&enabled,out)==OF_REJECT);
    CHECK(of_store_select(d,"demo.sh",lookup,&enabled)<0);
    enabled=1; CHECK(of_store_select(d,"missing.sh",lookup,&enabled)<0);
    CHECK(of_store_select(d,"../../sh",lookup,&enabled)<0);
    CHECK(of_store_select(d,"a;touch pwned",lookup,&enabled)<0);
    char longname[67]; memset(longname,'a',66); longname[66]=0;
    CHECK(of_store_select(d,longname,lookup,&enabled)<0);
    CHECK(of_store_read(d,out)==0 && !strcmp(out,"demo.sh"));
    CHECK(of_store_select(d,"official",NULL,NULL)==0);
    CHECK(of_store_resolve(d,NULL,NULL,out)==OF_OFFICIAL);
    CHECK(!unlinkat(d,"official-forward",0));
    CHECK(!symlinkat("/etc/passwd",d,"official-forward"));
    CHECK(of_store_read(d,out)<0); CHECK(of_store_select(d,"official",NULL,NULL)<0);
    CHECK(!unlinkat(d,"official-forward",0));
    put(d,"official\n",0644); CHECK(!fchmodat(d,"official-forward",0644,0));
    CHECK(of_store_read(d,out)<0); CHECK(of_store_select(d,"official",NULL,NULL)<0);
    CHECK(!unlinkat(d,"official-forward",0));
    put(d,"official\ntrailing",0600); CHECK(of_store_read(d,out)<0);
    CHECK(!unlinkat(d,"official-forward",0));
    CHECK(of_store_select(d,"official",NULL,NULL)==0);
    int old=openat(d,"official-forward",O_RDONLY); CHECK(old>=0);
    pid_t child=fork(); CHECK(child>=0);
    if(!child){for(int i=0;i<2000;i++){char v[65]; if(of_store_read(d,v)|| (strcmp(v,"official")&&strcmp(v,"demo.sh"))) _exit(1);} _exit(0);}
    for(int i=0;i<300;i++) CHECK(!of_store_select(d,i%2?"official":"demo.sh",lookup,&enabled));
    int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    char original[20]={0}; CHECK(read(old,original,sizeof original)==9 && !memcmp(original,"official\n",9)); close(old);
    CHECK(!fchmod(d,0777)); CHECK(of_store_read(d,out)<0); CHECK(of_store_select(d,"official",NULL,NULL)<0); CHECK(!fchmod(d,0700));
    CHECK(!unlinkat(d,"official-forward",0)); close(d); CHECK(!rmdir(dir)); puts("official forwarding store: PASS");
}
