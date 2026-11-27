#define _GNU_SOURCE
#include "declaration-reader.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Like matcher-preflight.c, inspect the existing private parse_pat rather
 * than implementing a second template grammar. Exactly one matcher TU. */
#include "template-matcher.c"

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_mode==b->st_mode &&
        a->st_nlink==b->st_nlink && a->st_uid==b->st_uid && a->st_gid==b->st_gid &&
        a->st_size==b->st_size && a->st_mtim.tv_sec==b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec==b->st_mtim.tv_nsec && a->st_ctim.tv_sec==b->st_ctim.tv_sec &&
        a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int decl_read_fd_impl(int fd, struct cfg *out,cfg_list_approve approve,void *ctx) {
    struct stat before, after;
    if(!out || fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_size<0)
        return CFG_INVALID;
    if((uintmax_t)before.st_size>CFG_DECL_BYTES) return CFG_CAPACITY;
    size_t n=(size_t)before.st_size, pos=0;
    char *raw=malloc(n+1);
    struct cfg *next=malloc(sizeof(*next));
    if(!raw || !next) { free(raw); free(next); return CFG_CAPACITY; }
    int rc=CFG_INVALID;
    while(pos<n) {
        ssize_t got=pread(fd,raw+pos,n-pos,(off_t)pos);
        if(got<0 && errno==EINTR) continue;
        if(got<=0 || (size_t)got>n-pos) goto done;
        pos+=(size_t)got;
    }
    char extra;
    ssize_t got;
    do { got=pread(fd,&extra,1,(off_t)n); } while(got<0 && errno==EINTR);
    if(got!=0 || fstat(fd,&after) || !same_file(&before,&after)) goto done;
    raw[n]=0;
    rc=approve?cfg_parse_rows(raw,n,next,approve,ctx):cfg_parse(raw,n,next);
    if(!rc) *out=*next;
done:
    free(next); free(raw); return rc;
}
int decl_read_fd(int fd,struct cfg *out) {return decl_read_fd_impl(fd,out,NULL,NULL);}
int decl_read_fd_rows(int fd,struct cfg *out,cfg_list_approve approve,void *ctx) {
    return approve?decl_read_fd_impl(fd,out,approve,ctx):CFG_INVALID;
}
static int phrase(struct decl_table *t, enum decl_scope scope,
                  const char *a, const char *b, int line,int typed) {
    while(a<b && (*a==' ' || *a=='\t')) a++;
    while(b>a && (b[-1]==' ' || b[-1]=='\t')) b--;
    size_t n=(size_t)(b-a);
    if(!n) return CFG_INVALID;
    if(n>EXPR_BYTES) return CFG_CAPACITY;
    char text[EXPR_BYTES+1]; memcpy(text,a,n); text[n]=0;
    struct expr_program *program=malloc(sizeof(*program));
    if(!program) return CFG_CAPACITY;
    int status=lx06_expr_compile(text,n,program);
    for(unsigned s=0;!status && s<program->slots;s++)
        if(program->lists[s]&&!typed) status=EXPR_LIST_PENDING;
    free(program);
    if(status) return status;
    if(t->count[scope]>=CFG_CANDIDATES) return CFG_CAPACITY;
    size_t offset=0;
    while(offset<t->used) {
        if(!strcmp(t->phrases+offset,text)) break;
        offset+=strlen(t->phrases+offset)+1;
    }
    if(offset==t->used) {
        if(n+1>sizeof(t->phrases)-t->used) return CFG_CAPACITY;
        memcpy(t->phrases+offset,text,n+1); t->used+=n+1;
    }
    struct decl_item *item=&t->items[scope][t->count[scope]++];
    item->offset=(uint32_t)offset; item->line=line;
    return CFG_OK;
}
static int list(struct decl_table *t, enum decl_scope scope, const char *value,
                char separator, const char *end, int line,int typed) {
    if(!value || value==end || !*value) return CFG_OK;
    struct expr_span *spans=calloc(EXPR_ITEMS,sizeof(*spans));
    if(!spans) return CFG_CAPACITY;
    size_t count=0;
    int rc=lx06_expr_split(value,(size_t)(end-value),separator,spans,EXPR_ITEMS,&count);
    for(size_t i=0;!rc && i<count;i++) {
        int id=scope==DECL_CTL?line:(int)t->count[scope];
        const char *p=value+spans[i].offset;
        rc=phrase(t,scope,p,p+spans[i].length,id,typed);
    }
    free(spans); return rc;
}
int decl_compile_entry_channels(const struct cfg *c, struct decl_table *out) {
    if(!c || !out) return CFG_INVALID;
    struct decl_table *t=calloc(1,sizeof(*t)); if(!t) return CFG_CAPACITY;
    t->source=c;
    const char *keys[]={"小爱通道唤醒词","LX06-LOCAL-CHANNEL"};
    int rc=CFG_OK;
    for(int s=0;s<2;s++) {
        const char *v=cfg_value(c,keys[s]);
        rc=list(t,(enum decl_scope)s,v,s==DECL_OFFICIAL?',':'|',v?v+strlen(v):NULL,0,0);
        if(rc) break;
    }
    if(!rc) *out=*t;
    free(t); return rc;
}
int decl_compile_channels(const struct cfg *c, struct decl_table *out) {
    if(!c || c->app_count) return CFG_INVALID;
    return decl_compile_entry_channels(c,out);
}
static int decl_compile_impl(const struct cfg *c, const char *app, struct decl_table *out,cfg_list_approve approve,void *ctx) {
    if(!c || !app || !out) return CFG_INVALID;
    size_t a=0; while(a<c->app_count && strcmp(c->apps[a],app)) a++;
    if(a==c->app_count) return CFG_INVALID;
    struct decl_table *t=calloc(1,sizeof(*t)); if(!t) return CFG_CAPACITY;
    t->source=c; strcpy(t->app,c->apps[a]);
    const char *keys[]={"小爱通道唤醒词","LX06-LOCAL-CHANNEL",NULL};
    char play[81]; int n=snprintf(play,sizeof(play),"LX06-%s-PLAY-TPL",app);
    int rc=CFG_INVALID;
    if(n<0 || (size_t)n>=sizeof(play)) goto done;
    for(char *p=play;*p;p++) if(*p>='a' && *p<='z') *p=(char)(*p-'a'+'A');
    keys[DECL_PLAY]=play;
    for(int s=0;s<DECL_CTL;s++) {
        const char *v=cfg_value(c,keys[s]);
        rc=list(t,(enum decl_scope)s,v,s==DECL_OFFICIAL?',':'|',v?v+strlen(v):NULL,0,0);
        if(rc) goto done;
    }
    char ctl_key[81];
    int key_size=snprintf(ctl_key,sizeof(ctl_key),"LX06-%s-CTL",app);
    if(key_size<0||(size_t)key_size>=sizeof(ctl_key))goto done;
    for(char *p=ctl_key;*p;p++)if(*p>='a'&&*p<='z')*p=(char)(*p-'a'+'A');
    for(size_t i=0;;i++) {
        const char *raw=cfg_ctl_row(c,app,i); if(!raw) break;
        struct cfg_row row;
        rc=cfg_parse_row_scoped(ctl_key,raw,&row,approve,ctx); if(rc) goto done;
        struct expr_span sides[2]; size_t used=0;
        rc=lx06_expr_split(raw,strlen(raw),'=',sides,2,&used); if(rc) goto done;
        const char *end=raw+sides[0].length;
        if(i>INT_MAX) { rc=CFG_CAPACITY; goto done; }
        rc=list(t,DECL_CTL,raw,'|',end,(int)i,approve!=NULL); if(rc) goto done;
    }
    *out=*t; rc=CFG_OK;
done:
    free(t); return rc;
}
int decl_compile(const struct cfg *c,const char *app,struct decl_table *out) {
    return decl_compile_impl(c,app,out,NULL,NULL);
}
int decl_compile_rows(const struct cfg *c,const char *app,struct decl_table *out,cfg_list_approve approve,void *ctx) {
    return approve?decl_compile_impl(c,app,out,approve,ctx):CFG_INVALID;
}
int decl_candidate(const struct decl_table *t, enum decl_scope scope, size_t index,
                   const char *script, struct lx06_match_candidate *out) {
    if(!t || !out || scope<0 || scope>=DECL_SCOPES || index>=t->count[scope]) return CFG_INVALID;
    const struct decl_item *item=&t->items[scope][index];
    struct lx06_match_candidate c={script,item->line,t->phrases+item->offset};
    *out=c; return CFG_OK;
}
const char *decl_ctl_row(const struct decl_table *t, size_t candidate) {
    if(!t || !t->source || candidate>=t->count[DECL_CTL]) return NULL;
    return cfg_ctl_row(t->source,t->app,(size_t)t->items[DECL_CTL][candidate].line);
}
#include "declaration-expression.inc"
int decl_match(const struct decl_table *t, enum decl_scope scope, const char *script,
               const char *text, struct lx06_match_result *out) {
    int branch;
    return decl_expr_match(t,scope,script,text,out,&branch);
}

/* The original static-branch positive checker borrows this same matcher TU;
 * no copied expression grammar or separate arbitration is introduced. */
static void ats_static_wipe(void *data,size_t size){volatile unsigned char *p=data;while(size--)*p++=0;}
#include "static-branch-review.inc"
int cfg_static_phrase_hit(const char *phrase,const char *word){
    struct gpb_hits hits={0};int rc=gpb_static(phrase,word,EXPR_MATCH_STEPS,&hits);
    return rc?-1:hits.matched?1:0;
}
