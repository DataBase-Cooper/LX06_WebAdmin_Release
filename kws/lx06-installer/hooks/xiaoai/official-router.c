/* One firmware service for all official-ASR script commands. Only installed
 * declarations are data; recognized speech is never evaluated as shell code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include "official-router-protocol.h"
#include "session-owner.h"
#include "script-audio-mode.h"
#include "glibc_ni_trap.h"
#include "../robust/keeper_client.h"
#include "../robust/script-policy.h"
#include "../../runtime/doubao/src/json.h"
#include "../../runtime/common/script-settings-store.h"
#include "template-matcher.c"
#include "official-bind.h"
#ifndef OR_APPS
#define OR_APPS "/data/lx06/apps"
#endif
#ifndef OR_LOG
#define OR_LOG "/tmp/mico_aivs_lab/instruction.log"
#endif
#ifndef OR_NATIVE
#define OR_NATIVE "/tmp/lx06-native-wake"
#endif
#ifndef OR_GATE_PID
#define OR_GATE_PID "/tmp/lx06-official-gate.pid"
#endif
#define ROUTES 64
#define TPL_MAX 64
#define DIALOGS OR_MAX_CANCELED
struct route {char word[128],name[65];int chat,root,ambiguous,managed,phone;};
static struct route routes[ROUTES];static int route_count;
struct tpl {char phrase[160],name[65];int chat,root,managed,line,phone;};
static struct tpl templates[TPL_MAX];static int template_count;
static char matched_name[65],prompt_kind[32];
/* No eviction during this boot: a delayed response cannot become audible
 * after DB_Play/new wake. Capacity exhaustion rejects further transfers. */
struct dialog {char id[96];int canceled,final;};
static struct dialog dialogs[DIALOGS];static int dialog_count;
struct observation {char id[96],marker[128];long long at;int final;};
static struct observation observations[32];static unsigned observation_next;
static int save_canceled(void) {
    uint32_t header[2]={OR_MAGIC,0};
    for(int i=0;i<dialog_count;i++)if(dialogs[i].canceled)header[1]++;
    int f=open(OR_CANCELED ".new",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(f<0)return 0;
    int ok=write(f,header,sizeof(header))==sizeof(header);
    for(int i=0;ok&&i<dialog_count;i++)if(dialogs[i].canceled)ok=write(f,dialogs[i].id,96)==96;
    close(f);
    return ok&&!rename(OR_CANCELED ".new",OR_CANCELED);
}
static void load_canceled(void) {
    int f=open(OR_CANCELED,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return;
    uint32_t header[2];char id[96];
    if(read(f,header,sizeof(header))!=sizeof(header)||header[0]!=OR_MAGIC||header[1]>DIALOGS){close(f);return;}
    for(unsigned i=0;i<header[1];i++) {
        if(read(f,id,sizeof(id))!=sizeof(id))break;
        if(!memchr(id,0,96)||!id[0])continue;
        struct dialog *d=&dialogs[dialog_count++];strcpy(d->id,id);d->canceled=d->final=1;
    }
    close(f);
}
static int logfd=-1,notifyfd=-1,started,discard_line;static ino_t logino;
static char linebuf[16384];static size_t used;
static pid_t worker;
static long long next_refresh;
static volatile sig_atomic_t quit;
static void stop(int s){(void)s;quit=1;}
static void note(const char *event,const char *value){fprintf(stderr,"official-router: %s %s\n",event,value);fflush(stderr);}
/* Remove only spacing and sentence punctuation. Keep all words and negation. */
static int normalize(const char *in,char out[128]) {
    size_t n=0;
    while(*in) {
        unsigned char c=(unsigned char)*in;
        if(c<128 && (c==' ' || c=='\t' || c=='\r' || c=='\n' || strchr(".,!?;:",c))) {in++;continue;}
        const char *p[]={"，","。","！","？","；","：","、","　"};int skip=0;
        for(unsigned i=0;i<sizeof(p)/sizeof(*p);i++) if(!strncmp(in,p[i],strlen(p[i]))) {in+=strlen(p[i]);skip=1;break;}
        if(skip) continue;
        if(c<32 || n+1>=128) return 0;
        out[n++]=*in++;
    }
    out[n]=0;return n>0;
}
static int name_ok(const char *s) {
    size_t n=strlen(s);if(!n || n>64 || s[0]=='.') return 0;
    for(size_t i=0;i<n;i++) if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
            (s[i]>='0'&&s[i]<='9')||strchr("._-",s[i])))return 0;
    return 1;
}
static void add_template(const char *phrase,const char *name,int chat,int root) {
    int line=0;
    if(!phrase || template_count>=TPL_MAX || strlen(phrase)>=sizeof(templates[0].phrase)) return;
    for(int i=0;i<template_count;i++) if(!strcmp(templates[i].name,name)) line=templates[i].line+1;
    struct tpl *t=&templates[template_count++];
    memset(t,0,sizeof(*t));
    snprintf(t->phrase,sizeof(t->phrase),"%s",phrase);
    snprintf(t->name,sizeof(t->name),"%s",name);
    t->chat=chat;t->root=root;t->line=line;
}
static int exact_claimed(const char *word) {
    for(int i=0;i<route_count;i++) if(!strcmp(routes[i].word,word)) return 1;
    return 0;
}
/* 1 = synthesized route, 0 = no template hit, -1 = cross-script reject. */
static int template_resolve(const char *word,struct route *out) {
    struct lx06_match_candidate cands[TPL_MAX];
    struct lx06_match_result mr;
    if(!template_count || exact_claimed(word)) return 0;
    for(int i=0;i<template_count;i++) {
        cands[i].script=templates[i].name;
        cands[i].line=templates[i].line;
        cands[i].phrase=templates[i].phrase;
    }
    if(lx06_template_match(word,cands,template_count,&mr)) return 0;
    if(mr.kind==LX06_MATCH_REJECT || mr.kind==LX06_MATCH_AMBIGUOUS) {
        snprintf(prompt_kind,sizeof(prompt_kind),"reject");
        return -1;
    }
    if((mr.kind!=LX06_MATCH_TEMPLATE && mr.kind!=LX06_MATCH_EXACT) || mr.index<0 || mr.index>=template_count) return 0;
    memset(out,0,sizeof(*out));
    snprintf(out->name,sizeof(out->name),"%s",templates[mr.index].name);
    snprintf(out->word,sizeof(out->word),"%s",word);
    out->chat=templates[mr.index].chat;
    out->root=templates[mr.index].root;
    out->managed=templates[mr.index].managed;
    out->phone=templates[mr.index].phone;
    return 1;
}
static void add_aliases(char *text,const char *name,int chat,int root) {
    /* Both Chinese and ASCII commas accepted. */
    for(char *p=text;(p=strstr(p,"，"));) { *p=',';memmove(p+1,p+3,strlen(p+3)+1);p++; }
    char *save,*s=strtok_r(text,",",&save);
    while(s) {
        char word[128];
        if(strchr(s,'{')) { add_template(s,name,chat,root); s=strtok_r(NULL,",",&save); continue; }
        if(normalize(s,word)) {
            int i;for(i=0;i<route_count;i++)if(!strcmp(word,routes[i].word))break;
            if(i<route_count) {if(strcmp(name,routes[i].name)||chat!=routes[i].chat)routes[i].ambiguous=1;}
            else if(route_count<ROUTES) {
                struct route *r=&routes[route_count++];
                memset(r,0,sizeof(*r));strcpy(r->word,word);strcpy(r->name,name);r->chat=chat;r->root=root;
            }
        }
        s=strtok_r(NULL,",",&save);
    }
}
static void registry(void) {
    route_count=0;template_count=0;DIR *d=opendir(OR_APPS);if(!d)return;
    struct dirent *de;
    while((de=readdir(d))) {
        if(!name_ok(de->d_name))continue;
        int fd=openat(dirfd(d),de->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        struct stat st;
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&0022)){close(fd);continue;}
        char head[8193];ssize_t n=read(fd,head,8192);close(fd);if(n<=0)continue;head[n]=0;
        struct lx06_script_audio_metadata audio=lx06_script_audio_parse((const unsigned char *)head,(size_t)n);
        if(audio.mode==LX06_AUDIO_INVALID || (audio.mode==LX06_AUDIO_PHONE_BRIDGE&&audio.phone_app!=LX06_PHONE_DOUBAO))continue;
        int phone=audio.mode==LX06_AUDIO_PHONE_BRIDGE&&audio.phone_app==LX06_PHONE_DOUBAO;
        char aliases[1024]="",actions[1024]="";int root=0,local=0,configurable=0,managed=0;
        char *save,*s=strtok_r(head,"\n",&save);
        while(s) {
            size_t l=strlen(s);while(l && (s[l-1]=='\r'||s[l-1]==' '||s[l-1]=='\t'))s[--l]=0;
            if(!strcmp(s,"# LX06-RUN: root")||!strcmp(s,"isroot=1")||!strcmp(s,"# isroot=1"))root=1;
            if(!strcmp(s,"# LX06-LOCALWAKEUP: on"))local=1;
            if(!strcmp(s,"# LX06-SETTINGS: official_wake"))configurable|=1;
            if(!strcmp(s,"# LX06-SETTINGS: official_command"))configurable|=2;
            if(!strcmp(s,"# LX06-SESSION-MANAGED: on"))managed=1;
            const char *a=phone?"# 小爱通道唤醒词: ":"# 小爱通道唤醒词=",*b="# 小爱通道指令=";
            if(!strncmp(s,a,strlen(a)) && strlen(s+strlen(a))<sizeof(aliases))strcpy(aliases,s+strlen(a));
            if(!strncmp(s,b,strlen(b)) && strlen(s+strlen(b))<sizeof(actions))strcpy(actions,s+strlen(b));
            s=strtok_r(NULL,"\n",&save);
        }
        if(configurable) {
            int dir=settings_dir(0),r=0;char raw[SETTINGS_CAP+1];
            if(dir>=0){r=settings_read(dir,de->d_name,raw);close(dir);}
            else if(errno!=ENOENT)r=-1;
            if(r>0) {
                xz_json_doc j;
                if(xz_json_parse(&j,raw,strlen(raw))||j.tokens[0].type!=XZ_JSON_OBJECT)r=-1;
                else {
                    if((configurable&1) && xz_json_object_get(&j,0,"official_wake")>=0 &&
                       xz_json_get_string(&j,0,"official_wake",aliases,sizeof(aliases)))aliases[0]=0;
                    if((configurable&2) && xz_json_object_get(&j,0,"official_command")>=0 &&
                       xz_json_get_string(&j,0,"official_command",actions,sizeof(actions)))actions[0]=0;
                }
            }
            /* A missing override preserves defaults. An explicit empty value
             * disables aliases. Corrupt/untrusted state never revives old ones. */
            if(r<0 && (configurable&1))aliases[0]=0;
            if(r<0 && (configurable&2))actions[0]=0;
        }
        if(local && *aliases)add_aliases(aliases,de->d_name,1,root);
        if(*actions)add_aliases(actions,de->d_name,0,root);
        for(int i=0;i<route_count;i++)if(!strcmp(routes[i].name,de->d_name)&&routes[i].chat){routes[i].managed=managed;routes[i].phone=phone;}
        for(int i=0;i<template_count;i++)if(!strcmp(templates[i].name,de->d_name)){templates[i].managed=managed;templates[i].phone=phone;}
    }
    closedir(d);
}
static struct route *match(const char *word) {
    for(int i=0;i<route_count;i++)if(!routes[i].ambiguous&&!strcmp(routes[i].word,word))return &routes[i];
    return NULL;
}
static struct dialog *find_dialog(const char *id,int create) {
    if(!*id || strlen(id)>=96)return NULL;
    for(int i=0;i<dialog_count;i++)if(!strcmp(id,dialogs[i].id))return &dialogs[i];
    if(!create||dialog_count==DIALOGS)return NULL;
    struct dialog *d=&dialogs[dialog_count++];memset(d,0,sizeof(*d));strcpy(d->id,id);return d;
}
static int snapshot(char *out,size_t cap) {
    int fd=open(OR_NATIVE,O_RDONLY|O_CLOEXEC);if(fd<0){out[0]=0;return 0;}
    ssize_t n=read(fd,out,cap-1);close(fd);if(n<0)n=0;out[n]=0;return (int)n;
}
static int gate_ready(void) {
    unsigned pid;unsigned long long ticks;
    FILE *f=fopen(OR_GATE_PID,"r");if(!f)return 0;
    int ok=fscanf(f,"%u %llu",&pid,&ticks)==2;fclose(f);
    return ok&&pid>1&&ticks&&lx06_process_ticks((pid_t)pid)==ticks;
}
static int run_stop(void) {
    pid_t p=fork();if(p<0)return -1;
    if(!p) {
        int null=open("/dev/null",O_WRONLY);if(null>=0){dup2(null,1);dup2(null,2);close(null);}
        execlp("ubus","ubus","-t","1","call","mediaplayer","player_play_operation",
               "{\"media\":\"tts\",\"action\":\"stop\"}",(char *)NULL);_exit(127);
    }
    int status;while(waitpid(p,&status,0)<0)if(errno!=EINTR)return -1;
    return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:-1;
}
static int dispatch(struct route route,const char *wake,const char *marker) {
    if(route.phone) {
        if(!route.chat||!wake[0]||strlen(wake)>96)return 1;
        for(const unsigned char *p=(const unsigned char *)wake;*p;p++)if(*p<32||*p==127)return 1;
    }
    if(!lx06_scripts_allowed()) {note("scripts-disabled",route.name);return 1;}
    char current[128];
    snapshot(current,sizeof(current));if(strcmp(marker,current))return 1;
    struct lx06_session_record owner;
    if(lx06_session_read(&owner)) {
        if(lx06_session_preempt_expected(&owner)<0)return 1;
        long long end=lk_now_ms()+2500;
        while(lx06_session_busy()&&lk_now_ms()<end)usleep(20000);
        if(lx06_session_busy())return 1;
    }
    /* No unbounded player waits. Never use VOIP, global mute or engine restart. */
    if(run_stop()) {note("stop-failed",route.name);return 1;}
    snapshot(current,sizeof(current));if(strcmp(marker,current)){note("new-wake-canceled",route.name);return 1;}
    if(!lx06_scripts_allowed())return 1;
    registry();struct route *fresh=match(wake);struct route again;
    if(!fresh){int tr=template_resolve(wake,&again);if(tr==1)fresh=&again;}
    if(!fresh||strcmp(fresh->name,route.name)||fresh->chat!=route.chat||fresh->root!=route.root||fresh->managed!=route.managed||fresh->phone!=route.phone)return 1;
    struct lk_request q={.magic=LK_MAGIC,.action=LK_STATUS};struct lk_reply reply;
    if(lk_call(&q,&reply,300)||reply.safe_mode)return 1;
    for(unsigned i=0;i<reply.jobs;i++)if(!strcmp(reply.job[i].name,route.name)) {
        if(!route.chat){note("action-already-running",route.name);return 1;}
        char dst[192],tmp[208],event[192];
        snprintf(dst,sizeof(dst),"/tmp/lx06-wake-%s.event",route.name);
        snprintf(tmp,sizeof(tmp),"%s.router",dst);
        if(route.phone)snprintf(event,sizeof(event),"%s\t0\t%.2f\tofficial_asr\n",wake,lk_now_ms()/1000.0);
        else snprintf(event,sizeof(event),"%s 0 %.2f\n",wake,lk_now_ms()/1000.0);
        int f=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(f<0)return 1;
        int ok=write(f,event,strlen(event))==(ssize_t)strlen(event);close(f);
        if(!ok||rename(tmp,dst))return 1;
        note("mailbox",route.name);return 0;
    }
    memset(&q,0,sizeof(q));q.magic=LK_MAGIC;q.action=LK_START;q.flags=route.root?LK_ROOT:0;
    /* Managed conversation scripts enforce their configurable deadline inside
     * session-run (including owned group cleanup). A competing keeper timeout
     * would quarantine unrelated scripts and cap user-selected long sessions. */
    q.timeout_sec=route.chat?(route.managed?0:960):30;
    strcpy(q.name,route.name);q.argc=4;strcpy(q.argv[0],"/bin/sh");
    snprintf(q.argv[1],sizeof(q.argv[1]),OR_APPS "/%s",route.name);
    if(route.phone) {
        q.argc=5;strcpy(q.argv[2],"--phone-wakeup");strcpy(q.argv[3],"official_asr");strcpy(q.argv[4],wake);
    } else {strcpy(q.argv[2],route.chat?"--wakeup":"--official-command");strcpy(q.argv[3],wake);}
    if(lk_call(&q,&reply,500)){note("start-failed",route.name);return 1;}
    note("started",route.name);return 0;
}
static void instruction(const char *text,size_t len) {
    static xz_json_doc j;
    if(xz_json_parse(&j,text,len))return;
    int h=xz_json_object_get(&j,j.root,"header"),p=xz_json_object_get(&j,j.root,"payload");
    char ns[64],name[64],id[96],speech[512],word[128];
    if(xz_json_get_string(&j,h,"namespace",ns,sizeof(ns))||strcmp(ns,"SpeechRecognizer")||
       xz_json_get_string(&j,h,"name",name,sizeof(name))||strcmp(name,"RecognizeResult")||
       xz_json_get_string(&j,h,"dialog_id",id,sizeof(id)))return;
    /* Auxiliary ASR is never an official command, even if a diagnostic SDK
     * build exposes its lifecycle events in the instruction log. */
    if(!strncmp(id,"lx06-ctrl-",10))return;
    struct observation *seen=NULL;
    for(unsigned i=0;i<32;i++)if(!strcmp(observations[i].id,id)){seen=&observations[i];break;}
    if(!seen){seen=&observations[observation_next++%32];memset(seen,0,sizeof(*seen));strcpy(seen->id,id);snapshot(seen->marker,sizeof(seen->marker));seen->at=lk_now_ms();}
    int f=xz_json_object_get(&j,p,"is_final");
    if(f<0 || j.tokens[f].end-j.tokens[f].start!=4 || memcmp(text+j.tokens[f].start,"true",4))return;
    char current_marker[128];snapshot(current_marker,sizeof(current_marker));
    if(seen->final||strcmp(seen->marker,current_marker)||lk_now_ms()-seen->at>30000)return;
    seen->final=1;
    /* Deduplicate every final, including unmatched finals. Corrections/partials
     * never execute a physical action and never provisionally discard audio. */
    struct dialog *old=find_dialog(id,0);if(old&&old->final)return;
    int results=xz_json_object_get(&j,p,"results");
    if(results<0||j.tokens[results].type!=XZ_JSON_ARRAY)return;
    if(xz_json_get_string(&j,j.tokens[results].first_child,"text",speech,sizeof(speech))||!normalize(speech,word))return;
    struct route synthesized,*r=match(word);
    if(!r && template_count) {
        int tr=template_resolve(word,&synthesized);
        if(tr<0) {
            note("prompt","reject");
            /* Block the official reply without starting a script. A dead gate
             * must not record a cancel it cannot enforce. */
            if(lx06_scripts_allowed() && gate_ready()) {
                struct dialog *blocked=find_dialog(id,1);
                if(blocked) {
                    blocked->final=1;blocked->canceled=1;
                    if(!save_canceled()){blocked->canceled=0;note("gate-state-failed","reject");}
                }
            }
            return;
        }
        if(tr==1) r=&synthesized;
    }
    if(!r)return;
    /* A bound official keyword does not forward other scripts. An empty
     * channel on the bound script is passthrough at wake time, so a later
     * ASR final is not a second sentence for anyone else. */
    {
        char bind_id[65];
        int bind_state=lx06_official_bind_decide_at(LX06_OFFICIAL_BIND_PATH,OR_APPS,bind_id,sizeof(bind_id));
        if(bind_state==LX06_BIND_ARMED &&
           (strcmp(r->name,bind_id) || !lx06_local_channel_armed_at(OR_APPS,bind_id)))
            return;
    }
    snprintf(matched_name,sizeof(matched_name),"%s",r->name);
    /* Reject BEFORE canceling official playback or preempting a conversation. */
    if(!lx06_scripts_allowed()){note("scripts-disabled",r->name);return;}
    if(worker||!gate_ready()){note("unavailable",r->name);return;}
    struct dialog *d=find_dialog(id,1);if(!d){note("dialog-capacity",r->name);return;}
    d->final=1;d->canceled=1;
    if(!save_canceled()){d->canceled=0;note("gate-state-failed",r->name);return;}
#ifdef OR_TEST
    note("matched",r->name);
#else
    worker=fork();
    if(worker==0){if(logfd>=0)close(logfd);if(notifyfd>=0)close(notifyfd);_exit(dispatch(*r,word,current_marker));}
    if(worker<0){worker=0;d->canceled=0;note("fork-failed",r->name);}
#endif
}
/* Start at EOF. Rotation/truncation starts a new stream, never replay the
 * previous boot's command log. Limit work per event-loop iteration. */
static void readlog(void) {
    struct stat st;if(stat(OR_LOG,&st))return;
    if(logfd<0||st.st_ino!=logino) {
        if(logfd>=0)close(logfd);
        logfd=open(OR_LOG,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(logfd<0)return;
        logino=st.st_ino;used=0;discard_line=0;
        if(!started){lseek(logfd,0,SEEK_END);started=1;}
    }
    off_t pos=lseek(logfd,0,SEEK_CUR);if(pos>st.st_size){lseek(logfd,0,SEEK_SET);used=0;discard_line=0;}
    char buf[4096];int budget=65536;ssize_t n;
    while(budget>0 && (n=read(logfd,buf,sizeof(buf)))>0) {
        budget-=(int)n;
        for(ssize_t i=0;i<n;i++) {
            if(buf[i]=='\n') {if(!discard_line&&used)instruction(linebuf,used);used=0;discard_line=0;}
            else if(!discard_line) {if(used<sizeof(linebuf))linebuf[used++]=buf[i];else{used=0;discard_line=1;}}
        }
    }
}
/* Consume denied FIFO via its existing HTTP server. nano_httpd retains FIFO
 * ownership and emits its normal fifo_finished notification; no renames or
 * unlink, no PCM buffers, and no extra long-lived process per reply. */
struct drain {int fd;long long until;};static struct drain drains[4];
static int drain_url(const char *url) {
    const char *path=NULL;
    const char *prefix[]={"http://localhost:8888/","http://127.0.0.1:8888/"};
    for(unsigned i=0;i<2;i++)if(!strncmp(url,prefix[i],strlen(prefix[i])))path=url+strlen(prefix[i])-1;
    if(!path || (strncmp(path,"/tmp/tts/",9)&&strncmp(path,"//tmp/tts/",10)) ||
       strstr(path,"..")||strpbrk(path,"\r\n "))return 0;
    int slot;for(slot=0;slot<4;slot++)if(drains[slot].fd<0)break;if(slot==4)return 0;
    int s=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(s<0)return 0;
    struct sockaddr_in sa={.sin_family=AF_INET,.sin_port=htons(8888),.sin_addr={htonl(INADDR_LOOPBACK)}};
    if(connect(s,(void *)&sa,sizeof(sa)) && errno!=EINPROGRESS){close(s);return 0;}
    struct pollfd poller={s,POLLOUT,0};
    char req[640];int n=snprintf(req,sizeof(req),"GET %s HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n",path);
    if(poll(&poller,1,30)<=0 || send(s,req,(size_t)n,MSG_NOSIGNAL)!=n){close(s);return 0;}
    drains[slot]=(struct drain){s,lk_now_ms()+120000};return 1;
}
static void drain_poll(void) {
    char discard[4096];long long now=lk_now_ms();
    for(int i=0;i<4;i++)if(drains[i].fd>=0) {
        ssize_t n=recv(drains[i].fd,discard,sizeof(discard),MSG_DONTWAIT);
        if(n==0 || (n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)||now>=drains[i].until){close(drains[i].fd);drains[i].fd=-1;}
    }
}
static void query(int server) {
    int fd=accept4(server,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);if(fd<0)return;
    struct ucred peer;socklen_t size=sizeof(peer);struct or_query q;struct or_reply r={OR_MAGIC,0};
    struct pollfd p={fd,POLLIN,0};
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)||peer.uid!=0||poll(&p,1,10)<=0||
       recv(fd,&q,sizeof(q),MSG_TRUNC)!=sizeof(q)||q.magic!=OR_MAGIC||
       !memchr(q.dialog,0,sizeof(q.dialog))||!memchr(q.url,0,sizeof(q.url))){close(fd);return;}
    readlog(); /* Close the watcher-versus-player race before answering. */
    struct dialog *d=find_dialog(q.dialog,0);
    if(q.action==OR_PLAY&&d&&d->canceled) {
        /* Deny even on drain failure: never leak a canceled response. Log the
         * resource failure so the failed transfer can be diagnosed. */
        r.blocked=1;if(!drain_url(q.url))note("drain-failed",q.dialog);
        note("blocked",q.dialog);
    }
    (void)send(fd,&r,sizeof(r),MSG_NOSIGNAL);close(fd);
}
#ifndef OR_TEST
int main(void) {
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    int lock=open(OR_SOCKET ".lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 2;
    int server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(server<0)return 2;
    struct sockaddr_un sa={.sun_family=AF_UNIX};strcpy(sa.sun_path,OR_SOCKET);
    unlink(OR_SOCKET);umask(077);if(bind(server,(void *)&sa,sizeof(sa))||listen(server,8))return 2;
    for(int i=0;i<4;i++)drains[i].fd=-1;
    load_canceled();registry();
    /* A log created after service startup contains new events, not history. */
    started=access(OR_LOG,F_OK)!=0;
    readlog();notifyfd=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);
    if(notifyfd>=0) {inotify_add_watch(notifyfd,"/tmp/mico_aivs_lab",IN_MODIFY|IN_CREATE|IN_MOVED_TO|IN_DELETE_SELF);inotify_add_watch(notifyfd,OR_APPS,IN_CLOSE_WRITE|IN_MOVED_TO|IN_DELETE);}
    note("ready","shared official command routing");
    while(!quit) {
        struct pollfd fds[6]={{server,POLLIN,0},{notifyfd,POLLIN,0}};
        for(int i=0;i<4;i++)fds[i+2]=(struct pollfd){drains[i].fd,POLLIN,0};
        poll(fds,6,100);
        if(fds[1].revents){char events[4096];while(read(notifyfd,events,sizeof(events))>0){};registry();}
        if(lk_now_ms()>=next_refresh){registry();next_refresh=lk_now_ms()+2000;}
        readlog();if(fds[0].revents)query(server);drain_poll();
        if(worker&&waitpid(worker,NULL,WNOHANG)==worker)worker=0;
    }
    if(worker){kill(worker,SIGTERM);waitpid(worker,NULL,0);}
    unlink(OR_SOCKET);return 0;
}
#endif
