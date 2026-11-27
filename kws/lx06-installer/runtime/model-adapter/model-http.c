/* A provider-neutral, per-dialog HTTPS connection owner.
 *
 * Uploads own all URLs, credentials and request/response JSON. This helper
 * accepts one private request at a time and forwards response bytes immediately
 * over a root-only UNIX socket. easy_reset deliberately retains libcurl's DNS,
 * TLS-session and connection cache; an aborted response is never reused until
 * libcurl has safely discarded its incomplete HTTP connection.
 *
 * Only the public curl 7.55.1 ABI is used. dlopen reuses the speaker's existing
 * libcurl/OpenSSL/c-ares libraries without a new firmware TLS dependency.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "vendor/curl/curl.h"
struct mho_client;
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
#include "model-http-owned.h"
#endif

/* The firmware has glibc 2.25; newer cross headers redirect plain stat to
 * 2.33 symbols. Keep the original ARM large-file entry points explicitly. */
#if defined(__arm__)
#define LX06_STATBUF struct stat64
extern int __fxstat64(int,int,struct stat64 *);
extern int __lxstat64(int,const char *,struct stat64 *);
#define LX06_FSTAT(fd,st) __fxstat64(3,(fd),(st))
#define LX06_LSTAT(path,st) __lxstat64(3,(path),(st))
#else
#define LX06_STATBUF struct stat
#define LX06_FSTAT(fd,st) fstat((fd),(st))
#define LX06_LSTAT(path,st) lstat((path),(st))
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#define URL_LIMIT 4096U
#define HEADER_LIMIT 16384U
#define BODY_LIMIT 1048576U
#define CHUNK_LIMIT 65536U

static volatile sig_atomic_t stopped;
static void stop_signal(int sig) {(void)sig;stopped=1;}
static int64_t now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}

struct curl_api {
    void *library;
    CURLcode (*global_init)(long);
    void (*global_cleanup)(void);
    CURL *(*easy_init)(void);
    void (*easy_cleanup)(CURL *);
    void (*easy_reset)(CURL *);
    CURLcode (*easy_setopt)(CURL *,CURLoption,...);
    CURLcode (*easy_getinfo)(CURL *,CURLINFO,...);
    CURLcode (*easy_perform)(CURL *);
    struct curl_slist *(*slist_append)(struct curl_slist *,const char *);
    void (*slist_free_all)(struct curl_slist *);
};
static struct curl_api curl;
static int load_curl(void) {
    curl.library=dlopen("libcurl.so.4",RTLD_NOW|RTLD_LOCAL);
    if(!curl.library)return 0;
#define LOAD(name) do {*(void **)(&curl.name)=dlsym(curl.library,"curl_" #name);if(!curl.name)return 0;}while(0)
    LOAD(global_init);LOAD(global_cleanup);LOAD(easy_init);LOAD(easy_cleanup);
    LOAD(easy_reset);LOAD(easy_setopt);LOAD(easy_getinfo);LOAD(easy_perform);
    LOAD(slist_append);LOAD(slist_free_all);
#undef LOAD
    return curl.global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
}

/* All socket I/O is interruptible. A dead parser closes its client; the next
 * CURL progress callback (also during a silent upstream) cancels that POST. */
static int wait_io(int fd,short events,int64_t deadline) {
    while(!stopped) {
        int64_t left=deadline-now_ms();if(left<=0)return 0;
        struct pollfd p={.fd=fd,.events=events};
        int rc=poll(&p,1,left>100?100:(int)left);
        if(rc<0){if(errno==EINTR)continue;return 0;}
        if(rc&&p.revents&events)return 1;
        if(rc&&p.revents&(POLLERR|POLLHUP|POLLNVAL))return 0;
    }
    return 0;
}
static int socket_write(int fd,const void *data,size_t size) {
    const unsigned char *p=data;int64_t deadline=now_ms()+10000;
    while(size&&!stopped) {
        ssize_t n=send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n>0){p+=n;size-=(size_t)n;continue;}
        if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&wait_io(fd,POLLOUT,deadline))continue;
        return 0;
    }
    return !size;
}
static int socket_read(int fd,void *data,size_t size,int64_t deadline) {
    unsigned char *p=data;
    while(size&&!stopped) {
        ssize_t n=recv(fd,p,size,MSG_DONTWAIT);
        if(n>0){p+=n;size-=(size_t)n;continue;}
        if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&wait_io(fd,POLLIN,deadline))continue;
        return 0;
    }
    return !size;
}
static int fd_write(int fd,const void *data,size_t size) {
    const unsigned char *p=data;
    while(size&&!stopped) {
        ssize_t n=write(fd,p,size);
        if(n>0){p+=n;size-=(size_t)n;}else if(n<0&&errno==EINTR)continue;else return 0;
    }
    return !size;
}
static int send_frame(int fd,unsigned char type,const void *data,uint32_t size) {
    unsigned char header[5]={type};uint32_t n=htonl(size);memcpy(header+1,&n,4);
    return socket_write(fd,header,sizeof(header))&&socket_write(fd,data,size);
}
static int done_frame(int fd,CURLcode code,long status) {
    if(fd<0)return 1;
    uint32_t data[2]={htonl((uint32_t)code),htonl((uint32_t)status)};
    return send_frame(fd,'E',data,sizeof(data));
}

static int private_dir(const char *path) {
    LX06_STATBUF st;
    if(LX06_LSTAT(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=0||(st.st_mode&07777)!=0700)return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    LX06_STATBUF opened;
    if(LX06_FSTAT(fd,&opened)||opened.st_dev!=st.st_dev||opened.st_ino!=st.st_ino){close(fd);return -1;}
    return fd;
}
static int socket_parent(const char *path) {
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t n=strlen(path);if(!n||n>=sizeof(parent)||path[0]!='/')return 0;
    memcpy(parent,path,n+1);char *slash=strrchr(parent,'/');if(!slash||slash==parent)return 0;
    *slash=0;int fd=private_dir(parent);if(fd<0)return 0;close(fd);return 1;
}
static int peer_root(int fd) {
    struct ucred cred;socklen_t size=sizeof(cred);
    return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&size)&&size==sizeof(cred)&&cred.uid==0;
}
static char *private_file(int dir,const char *name,size_t limit,size_t *length,int optional) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) {
        if(optional&&errno==ENOENT){*length=0;return calloc(1,1);}
        return NULL;
    }
    LX06_STATBUF st;
    if(LX06_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||
       (st.st_mode&07777)!=0600||st.st_size<0||(uint64_t)st.st_size>limit){close(fd);return NULL;}
    size_t n=(size_t)st.st_size;char *p=calloc(n+1,1);if(!p){close(fd);return NULL;}
    size_t used=0;
    while(used<n) {ssize_t got=read(fd,p+used,n-used);if(got>0)used+=(size_t)got;else if(got<0&&errno==EINTR)continue;else break;}
    close(fd);if(used!=n){free(p);return NULL;}*length=n;return p;
}
static int save_status(int dir,long status) {
    int fd=openat(dir,"status",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 0;
    LX06_STATBUF st;
    if(LX06_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||(st.st_mode&07777)!=0600){close(fd);return 0;}
    char text[16];int n=snprintf(text,sizeof(text),"%03ld\n",status);
    int good=n>0&&n<(int)sizeof(text)&&!ftruncate(fd,0)&&fd_write(fd,text,(size_t)n);
    close(fd);return good;
}

struct request {char *method,*url,*headers,*body;uint32_t lens[4],timeout;};
static void free_request(struct request *r) {free(r->method);free(r->url);free(r->headers);free(r->body);memset(r,0,sizeof(*r));}
static int request_valid(const struct request *r,int http) {
    if(!r->method||!r->url||!r->headers||!r->body||r->timeout<1||r->timeout>300)return 0;
    if(memchr(r->method,0,r->lens[0])||memchr(r->url,0,r->lens[1])||memchr(r->headers,0,r->lens[2]))return 0;
    if(strcmp(r->method,"POST")&&strcmp(r->method,"GET")&&strcmp(r->method,"HEAD"))return 0;
    /* curl 7.55 can replay a zero-body POST on a stale connection without
     * consulting SEEKFUNCTION. Account requests are nonempty JSON/forms. */
    if(!strcmp(r->method,"POST")&&!r->lens[3])return 0;
    if(strncmp(r->url,"https://",8)&&(strncmp(r->url,"http://",7)||!http))return 0;
    for(const unsigned char *p=(unsigned char *)r->url;*p;p++)if(*p<=32||*p==127)return 0;
    return !strcmp(r->method,"POST")||!r->lens[3];
}
static int directory_request(int dir,struct request *r,int http) {
    const char *names[]={"method","url","headers","body"};
    const size_t limits[]={8,URL_LIMIT,HEADER_LIMIT,BODY_LIMIT};
    char **parts[]={&r->method,&r->url,&r->headers,&r->body};
    for(int i=0;i<4;i++) {
        size_t n=0;*parts[i]=private_file(dir,names[i],limits[i],&n,0);
        if(!*parts[i])return 0;
        r->lens[i]=(uint32_t)n;
    }
    size_t n=0;char *budget=private_file(dir,"timeout",4,&n,1);r->timeout=120;
    if(!budget)return 0;
    if(n) {
        char *tail;long value=strtol(budget,&tail,10);
        if(*tail||value<1||value>300){free(budget);return 0;}
        r->timeout=(uint32_t)value;
    }
    free(budget);return request_valid(r,http);
}
/* This marker promises only that connect() can queue a request. The HEAD may
 * still be running. Keep an open directory and record the created inode so
 * shutdown cannot remove a pre-existing or replaced script-owned file. */
static int create_ready(int dir,LX06_STATBUF *own) {
    int fd=openat(dir,"ready",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return 0;
    int good=!LX06_FSTAT(fd,own)&&S_ISREG(own->st_mode)&&own->st_uid==0&&
        own->st_nlink==1&&(own->st_mode&07777)==0600;
    close(fd);return good;
}
static void remove_ready(int dir,const LX06_STATBUF *own) {
    int fd=openat(dir,"ready",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return;
    LX06_STATBUF current;
    if(!LX06_FSTAT(fd,&current)&&S_ISREG(current.st_mode)&&current.st_uid==0&&
       current.st_dev==own->st_dev&&current.st_ino==own->st_ino)unlinkat(dir,"ready",0);
    close(fd);
}
static int receive_request(int fd,struct request *r,int http) {
    unsigned char head[24];int64_t deadline=now_ms()+5000;
    if(!socket_read(fd,head,sizeof(head),deadline)||memcmp(head,"LXH1",4))return 0;
    uint32_t fields[5];memcpy(fields,head+4,sizeof(fields));
    for(int i=0;i<4;i++)r->lens[i]=ntohl(fields[i]);
    r->timeout=ntohl(fields[4]);
    if(r->lens[0]>8||r->lens[1]>URL_LIMIT||r->lens[2]>HEADER_LIMIT||r->lens[3]>BODY_LIMIT)return 0;
    char **parts[]={&r->method,&r->url,&r->headers,&r->body};
    for(int i=0;i<4;i++) {*(parts[i])=calloc((size_t)r->lens[i]+1,1);if(!*(parts[i])||!socket_read(fd,*(parts[i]),r->lens[i],deadline))return 0;}
    return request_valid(r,http);
}

/* A deliberately tiny curl-config subset, not an interpreter. Shared upload
 * auth files contain only quoted headers and user-agent; arbitrary curl
 * options, escapes introducing control bytes and header injection are refused.
 */
static int request_headers(char *config,struct curl_slist **out) {
    unsigned count=0;char *line=config;
    while(*line) {
        char *next=strchr(line,'\n');if(next)*next++=0;
        char *p=line;while(*p==' '||*p=='\t')p++;
        if(!*p||*p=='#'){line=next?next:p+strlen(p);continue;}
        int agent=0;
        if(!strncmp(p,"header",6)&&(p[6]==' '||p[6]=='\t'||p[6]=='='))p+=6;
        else if(!strncmp(p,"user-agent",10)&&(p[10]==' '||p[10]=='\t'||p[10]=='=')){p+=10;agent=1;}
        else return 0;
        while(*p==' '||*p=='\t')p++;
        if(*p++!='=')return 0;
        while(*p==' '||*p=='\t')p++;
        if(*p++!='"')return 0;
        char value[HEADER_LIMIT+1];size_t n=0;
        if(agent){memcpy(value,"User-Agent: ",12);n=12;}
        while(*p&&*p!='"') {
            unsigned char c=(unsigned char)*p++;
            if(c=='\\'){if(*p!='\\'&&*p!='"')return 0;c=(unsigned char)*p++;}
            if(c<32||c==127||n>=HEADER_LIMIT)return 0;
            value[n++]=(char)c;
        }
        if(*p++!='"')return 0;
        while(*p==' '||*p=='\t'||*p=='\r')p++;
        if(*p||!n||++count>64)return 0;
        value[n]=0;if(!strchr(value,':'))return 0;
        struct curl_slist *list=curl.slist_append(*out,value);if(!list)return 0;*out=list;
        line=next?next:p;
    }
    return 1;
}
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
#include "model-http-owned-state.inc"
#endif
struct transfer {int fd;CURL *easy;size_t received,body_size,body_read;const char *body;struct mho_client *owned;};
static int client_gone(int fd) {
    if(fd<0)return 0; /* An upload-defined HEAD has no IPC client to cancel. */
    struct pollfd p={.fd=fd,.events=POLLIN};int rc=poll(&p,1,0);
    if(rc<0&&errno!=EINTR)return 1;
    if(rc>0&&(p.revents&(POLLERR|POLLHUP|POLLNVAL)))return 1;
    if(rc>0&&(p.revents&POLLIN)){char c;ssize_t n=recv(fd,&c,1,MSG_PEEK|MSG_DONTWAIT);if(n==0||n>0)return 1;}
    return 0;
}
static int progress(void *opaque,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a;(void)b;(void)c;(void)d;struct transfer *t=opaque;
    return stopped||client_gone(t->fd)
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
        ||(t->owned&&!mho_live(t->owned))
#endif
        ;
}
static size_t request_bytes(char *data,size_t size,size_t count,void *opaque) {
    struct transfer *t=opaque;
    if(stopped||client_gone(t->fd)||(size&&count>SIZE_MAX/size)
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
       ||(t->owned&&!mho_live(t->owned))
#endif
       )return CURL_READFUNC_ABORT;
    size_t capacity=size*count,left=t->body_size-t->body_read,n=left<capacity?left:capacity;
    if(n){memcpy(data,t->body+t->body_read,n);t->body_read+=n;}
    return n;
}
static int request_rewind(void *opaque,curl_off_t offset,int origin) {
    (void)opaque;(void)offset;(void)origin;
    /* libcurl 7.55 otherwise silently replays POSTFIELDS on a stale reused
     * connection after the server may already have accepted the first POST. */
    return CURL_SEEKFUNC_FAIL;
}
static size_t response_bytes(char *data,size_t size,size_t count,void *opaque) {
    struct transfer *t=opaque;if(size&&count>SIZE_MAX/size)return 0;size_t n=size*count;
    if(stopped||client_gone(t->fd)||n>CHUNK_LIMIT)return 0;
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
    if(t->owned&&(!mho_live(t->owned)||n>MHO_BYTES-t->received))return 0;
#endif
    if(n>BODY_LIMIT-t->received)return 0;
    t->received+=n;
    long status=0;curl.easy_getinfo(t->easy,CURLINFO_RESPONSE_CODE,&status);
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
    if(t->owned) {
        if(status<200||status>=300)return n;
        memcpy(t->owned->response+t->owned->size,data,n);t->owned->size+=n;
        t->owned->response[t->owned->size]=0;return n;
    }
#endif
    /* Match curl --fail: provider error bodies never become spoken content. */
    if(t->fd<0||status<200||status>=300)return n;
    return send_frame(t->fd,'D',data,(uint32_t)n)?n:0;
}
static void timing(CURL *easy,long status,CURLcode code,int warm) {
    long connects=0;double dns=0,tcp=0,tls=0,first=0,total=0;
    curl.easy_getinfo(easy,CURLINFO_NUM_CONNECTS,&connects);
    curl.easy_getinfo(easy,CURLINFO_NAMELOOKUP_TIME,&dns);curl.easy_getinfo(easy,CURLINFO_CONNECT_TIME,&tcp);
    curl.easy_getinfo(easy,CURLINFO_APPCONNECT_TIME,&tls);curl.easy_getinfo(easy,CURLINFO_STARTTRANSFER_TIME,&first);
    curl.easy_getinfo(easy,CURLINFO_TOTAL_TIME,&total);
    fprintf(stderr,"model-http: %snew_connections=%ld dns_ms=%.0f connect_ms=%.0f tls_ms=%.0f first_ms=%.0f total_ms=%.0f http=%ld curl_code=%d\n",
            warm?"warm=1 ":"",connects,dns*1000,tcp*1000,tls*1000,first*1000,total*1000,status,(int)code);
}
static int perform_internal(int fd,CURL *easy,struct request *r,int allow_http,const char *ca,struct mho_client *owned) {
    int failed=1;
    struct curl_slist *headers=NULL;
    if(!request_headers(r->headers,&headers)){done_frame(fd,CURLE_BAD_FUNCTION_ARGUMENT,0);goto end;}
    /* These small JSON requests never benefit from a speculative one-second
     * Expect/100-continue wait. An explicit upload-owned Expect is preserved. */
    int has_expect=0;
    for(struct curl_slist *p=headers;p;p=p->next)if(!strncasecmp(p->data,"Expect:",7))has_expect=1;
    if(!has_expect){struct curl_slist *list=curl.slist_append(headers,"Expect:");if(!list){done_frame(fd,CURLE_OUT_OF_MEMORY,0);goto end;}headers=list;}
    curl.easy_reset(easy);struct transfer transfer={.fd=fd,.easy=easy,.body_size=r->lens[3],.body=r->body,.owned=owned};
#define SET(option,value) do{if(curl.easy_setopt(easy,option,value)!=CURLE_OK){done_frame(fd,CURLE_FAILED_INIT,0);goto end;}}while(0)
    SET(CURLOPT_URL,r->url);SET(CURLOPT_HTTPHEADER,headers);
    SET(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTPS|(allow_http?CURLPROTO_HTTP:0)));
    SET(CURLOPT_REDIR_PROTOCOLS,(long)CURLPROTO_HTTPS);SET(CURLOPT_FOLLOWLOCATION,0L);
    SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);if(ca)SET(CURLOPT_CAINFO,ca);
    SET(CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_1_1);SET(CURLOPT_NOSIGNAL,1L);
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
    if(owned) {
        if(!mho_live(owned))goto end;
        uint64_t now=mho_now(owned);if(!now||now>=owned->deadline)goto end;
        long left=(long)(owned->deadline-now);
        SET(CURLOPT_CONNECTTIMEOUT_MS,left);SET(CURLOPT_TIMEOUT_MS,left);
    } else
#endif
    {SET(CURLOPT_CONNECTTIMEOUT,12L);SET(CURLOPT_TIMEOUT,(long)r->timeout);}
    SET(CURLOPT_MAXCONNECTS,2L);
    SET(CURLOPT_TCP_NODELAY,1L);
    SET(CURLOPT_TCP_KEEPALIVE,1L);SET(CURLOPT_TCP_KEEPIDLE,30L);SET(CURLOPT_TCP_KEEPINTVL,15L);
    SET(CURLOPT_WRITEFUNCTION,response_bytes);SET(CURLOPT_WRITEDATA,&transfer);
    SET(CURLOPT_NOPROGRESS,0L);SET(CURLOPT_XFERINFOFUNCTION,progress);SET(CURLOPT_XFERINFODATA,&transfer);
    if(!strcmp(r->method,"POST")){
        SET(CURLOPT_POST,1L);SET(CURLOPT_POSTFIELDSIZE,(long)r->lens[3]);
        SET(CURLOPT_READFUNCTION,request_bytes);SET(CURLOPT_READDATA,&transfer);
        SET(CURLOPT_SEEKFUNCTION,request_rewind);SET(CURLOPT_SEEKDATA,&transfer);
    }
    else if(!strcmp(r->method,"HEAD"))SET(CURLOPT_NOBODY,1L);
    else SET(CURLOPT_HTTPGET,1L);
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
    if(owned) {
        if(!mho_live(owned))goto end;
        uint64_t now=mho_now(owned);if(!now||now>=owned->deadline)goto end;
        long left=(long)(owned->deadline-now);
        SET(CURLOPT_CONNECTTIMEOUT_MS,left);SET(CURLOPT_TIMEOUT_MS,left);
    }
#endif
    CURLcode code=curl.easy_perform(easy);long status=0;curl.easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
    if(owned){owned->code=code;owned->status=status;}
#endif
    timing(easy,status,code,fd<0&&!owned);if(fd>=0)done_frame(fd,code,status);
    /* Warming validates transport only. A completed anonymous HEAD returning
     * 401/403/405 can still retain a reusable connection; it says nothing
     * about account authorization and must never trigger a login mutation. */
    failed=code!=CURLE_OK||(fd<0&&!status);
#undef SET
end:
    /* Unset pointers before freeing request-owned memory; reset retains pool. */
    curl.easy_reset(easy);if(headers)curl.slist_free_all(headers);
    return failed;
}

static int perform(int fd,CURL *easy,struct request *r,int allow_http,const char *ca) {
    return perform_internal(fd,easy,r,allow_http,ca,NULL);
}
#ifdef LX06_MODEL_HTTP_OWNED_FIXTURE
#include "model-http-owned.inc"
#endif

static int serve(const char *path,int allow_http,const char *ca,const char *warm_path) {
    if(geteuid()!=0||!socket_parent(path)){fputs("model-http: private socket directory required\n",stderr);return 2;}
    LX06_STATBUF existing;if(!LX06_LSTAT(path,&existing)||errno!=ENOENT){fputs("model-http: socket path unavailable\n",stderr);return 2;}
    int warm_dir=-1;struct request warm={0};
    if(warm_path) {
        warm_dir=private_dir(warm_path);
        if(warm_dir<0||!directory_request(warm_dir,&warm,allow_http)||strcmp(warm.method,"HEAD")) {
            if(warm_dir>=0)close(warm_dir);
            free_request(&warm);
            fputs("model-http: private HEAD warm request required\n",stderr);return 2;
        }
    }
    CURL *easy=NULL;int listener=-1,result=2,bound=0,ready=0;LX06_STATBUF own={0},ready_own={0};
    if(!load_curl()){fputs("model-http: HTTPS runtime unavailable\n",stderr);goto end;}
    easy=curl.easy_init();if(!easy)goto end;
    listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)goto end;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    umask(077);
    if(bind(listener,(struct sockaddr *)&address,sizeof(address)))goto end;
    bound=1;
    if(LX06_LSTAT(path,&own))goto end;
    if(chmod(path,0600)||listen(listener,4))goto end;
    if(warm_dir>=0) {
        if(!create_ready(warm_dir,&ready_own)){fputs("model-http: warm ready marker unavailable\n",stderr);goto end;}
        ready=1;
        /* listen precedes HEAD: the greeting and TLS setup run together,
         * while a first question queues until this same pool is available.
         * Only scripts choose the safe non-inference endpoint and headers. */
        if(perform(-1,easy,&warm,allow_http,ca)) {
            fputs("model-http: warm_transport_failed=1\n",stderr);
            curl.easy_cleanup(easy);easy=curl.easy_init();if(!easy)goto end;
        }
        free_request(&warm);
    }
    result=0;
    while(!stopped) {
        struct pollfd p={.fd=listener,.events=POLLIN};int rc=poll(&p,1,200);
        if(rc<0){if(errno==EINTR)continue;result=1;break;}if(!rc)continue;
        int client=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(client<0)continue;
        if(peer_root(client)) {
            struct request request={0};
            if(receive_request(client,&request,allow_http)) {
                if(perform(client,easy,&request,allow_http,ca)) {
                    /* Failed rewinds can poison even an easy_reset handle.
                     * Keep successful pools only; a canceled/failed transfer
                     * gets a clean handle for the next independent question. */
                    curl.easy_cleanup(easy);easy=curl.easy_init();
                    if(!easy){free_request(&request);close(client);result=2;break;}
                }
            }
            else done_frame(client,CURLE_BAD_FUNCTION_ARGUMENT,0);
            free_request(&request);
        }
        close(client);
    }
end:
    if(listener>=0)close(listener);
    if(easy)curl.easy_cleanup(easy);
    if(curl.global_cleanup)curl.global_cleanup();
    if(ready)remove_ready(warm_dir,&ready_own);
    if(warm_dir>=0)close(warm_dir);
    free_request(&warm);
    if(bound){LX06_STATBUF current;if(!LX06_LSTAT(path,&current)&&S_ISSOCK(current.st_mode)&&current.st_uid==0&&
        current.st_dev==own.st_dev&&current.st_ino==own.st_ino)unlink(path);}
    if(curl.library)dlclose(curl.library);
    return result;
}

static int request_client(const char *socket_path,const char *directory) {
    int dir=private_dir(directory);if(geteuid()!=0||dir<0){if(dir>=0)close(dir);return 2;}
    int result=2,fd=-1;struct request r={0};
    /* Server alone decides whether its explicit host-test HTTP mode applies. */
    if(!directory_request(dir,&r,1)||!save_status(dir,0))goto end;
    LX06_STATBUF st;
    if(!socket_parent(socket_path)||LX06_LSTAT(socket_path,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=0||(st.st_mode&07777)!=0600){result=75;goto end;}
    fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0){result=75;goto end;}
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,socket_path);
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))||!peer_root(fd)){result=75;goto end;}
    /* After connect, even a partial request is never eligible for POST retry. */
    result=1;unsigned char head[24];memcpy(head,"LXH1",4);uint32_t fields[5];
    for(int i=0;i<4;i++)fields[i]=htonl(r.lens[i]);
    fields[4]=htonl(r.timeout);memcpy(head+4,fields,sizeof(fields));
    if(!socket_write(fd,head,sizeof(head)))goto end;
    char *parts[]={r.method,r.url,r.headers,r.body};
    for(int i=0;i<4;i++)if(!socket_write(fd,parts[i],r.lens[i]))goto end;
    int64_t deadline=now_ms()+((int64_t)r.timeout+20)*1000;
    while(!stopped) {
        unsigned char frame[5],data[CHUNK_LIMIT];uint32_t size;
        if(!socket_read(fd,frame,sizeof(frame),deadline))break;
        memcpy(&size,frame+1,4);size=ntohl(size);
        if(size>sizeof(data)||!socket_read(fd,data,size,deadline))break;
        if(frame[0]=='D'){if(!fd_write(STDOUT_FILENO,data,size))break;}
        else if(frame[0]=='E'&&size==8){uint32_t values[2];memcpy(values,data,sizeof(values));long status=ntohl(values[1]);
            if(!save_status(dir,status))break;
            result=ntohl(values[0])?1:(status>=200&&status<300?0:22);break;}
        else break;
    }
end:
    if(fd>=0)close(fd);
    free_request(&r);close(dir);return result;
}
#ifndef LX06_MODEL_HTTP_LIBRARY
int main(int argc,char **argv) {
    struct sigaction action={.sa_handler=stop_signal};sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);sigaction(SIGHUP,&action,NULL);signal(SIGPIPE,SIG_IGN);
    if(argc==4&&!strcmp(argv[1],"--request"))return request_client(argv[2],argv[3]);
    if(argc>=3&&!strcmp(argv[1],"--serve")) {
        int http=0;const char *ca=NULL,*warm=NULL;
        for(int i=3;i<argc;i++){if(!strcmp(argv[i],"--allow-http"))http=1;else if(!strcmp(argv[i],"--ca-file")&&i+1<argc)ca=argv[++i];
            else if(!strcmp(argv[i],"--warm")&&i+1<argc&&!warm)warm=argv[++i];else return 2;}
        return serve(argv[2],http,ca,warm);
    }
    fputs("usage: model-http --serve SOCKET [--warm PRIVATE_HEAD_DIR] [--ca-file FILE] [--allow-http]\n       model-http --request SOCKET PRIVATE_REQUEST_DIR\n",stderr);return 2;
}

#endif /* LX06_MODEL_HTTP_LIBRARY */
