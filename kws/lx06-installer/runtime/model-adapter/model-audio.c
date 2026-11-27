/* Native Xiaomi synthesis with owned playback plumbing.
 * --synthesize reads plain UTF-8 on stdin, requests play:0/save:1 and emits WAV.
 * This process NEVER asks the global media player to start/stop. A session core
 * can pipe WAV to its own aplay child and cancel that process group safely.
 * --decode is the same bounded decoder for reproducible offline tests.
 */
#define _GNU_SOURCE
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "vendor/minimp3/minimp3.h"
#include "../../hooks/robust/keeper_client.h"
#include "../doubao/src/json.c"
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__arm__)
#define LX06_STATBUF struct stat64
extern int __fxstat64(int,int,struct stat64*);
extern int __lxstat64(int,const char*,struct stat64*);
#define LX06_FSTAT(fd,st) __fxstat64(3,(fd),(st))
#define LX06_LSTAT(path,st) __lxstat64(3,(path),(st))
#else
#define LX06_STATBUF struct stat
#define LX06_FSTAT(fd,st) fstat((fd),(st))
#define LX06_LSTAT(path,st) lstat((path),(st))
#endif
#ifndef MODEL_UBUS
#define MODEL_UBUS "/bin/ubus"
#endif
#ifndef MODEL_TTS_PREFIX
#define MODEL_TTS_PREFIX "/tmp/tts/tts_"
#endif
#ifndef MODEL_TTS_OWNER
#define MODEL_TTS_OWNER "/tmp/lx06-model-tts-owner"
#endif
#ifndef MODEL_TTS_WAIT_MS
#define MODEL_TTS_WAIT_MS 15000
#endif
#ifndef MODEL_TTS_SLOW_FAILURE_MS
#define MODEL_TTS_SLOW_FAILURE_MS 5500
#endif
#ifndef MODEL_TTS_HTTP_PORT
#define MODEL_TTS_HTTP_PORT 8888
#endif
#ifndef MODEL_TTS_STREAM_MS
#define MODEL_TTS_STREAM_MS 15000
#endif
#ifndef MODEL_TTS_SOCKET
#define MODEL_TTS_SOCKET MODEL_TTS_OWNER "/stream.sock"
#endif
#define MAX_MP3 (512*1024)
#define MAX_SECONDS 30
static volatile sig_atomic_t stopped;
static volatile sig_atomic_t worker_stopping;
static void stop(int s){(void)s;stopped=1;}
static void stop_pending(int s){(void)s;worker_stopping=1;}
static int output_fd=1,output_overflow;
static int ready_fd=-1;
static size_t output_size,output_limit;
static void ready_open(void) {
    const char *value=getenv("MODEL_AUDIO_READY_FD");if(!value||!*value)return;
    char *end;long fd=strtol(value,&end,10);LX06_STATBUF st;
    if(*end||fd<3||fd>1024||LX06_FSTAT((int)fd,&st)||(!S_ISFIFO(st.st_mode)&&!S_ISSOCK(st.st_mode)))return;
    ready_fd=(int)fd;
}
static void ready_notify(void) {
    if(ready_fd<0)return;
    unsigned char ready=1;ssize_t n;
    do{n=write(ready_fd,&ready,1);}while(n<0&&errno==EINTR&&!stopped);
    close(ready_fd);ready_fd=-1;
}
static int write_bytes(const void*data,size_t n) {
    if(output_limit&&output_size+n>output_limit){output_overflow=1;return -1;}
    const char*p=data;
    while(n){if(stopped)return -1;ssize_t z=write(output_fd,p,n);if(z<0&&errno==EINTR)continue;if(z<=0)return -1;p+=z;n-=(size_t)z;output_size+=(size_t)z;if(output_fd==1&&output_size>44)ready_notify();}
    return 0;
}
static void little(unsigned char*p,unsigned v,int n){for(int i=0;i<n;i++){p[i]=(unsigned char)v;v>>=8;}}
static int wave_header(int rate,int channels,unsigned length) {
    unsigned char h[44]={0};memcpy(h,"RIFF",4);little(h+4,length+36,4);memcpy(h+8,"WAVEfmt ",8);
    little(h+16,16,4);little(h+20,1,2);little(h+22,channels,2);little(h+24,rate,4);
    little(h+28,rate*channels*2,4);little(h+32,channels*2,2);little(h+34,16,2);
    memcpy(h+36,"data",4);little(h+40,length,4);return write_bytes(h,sizeof(h));
}
/* Decode complete MP3 frames as they arrive, retaining at most 8 KiB encoded
 * bytes. A streaming WAV has an intentionally open-ended data length; aplay
 * stops at our pipe EOF. The actual emitted PCM still has a strict 30s bound.
 * Native Xiaomi MP3 is ordinary, non-free-format Layer III. Reject malformed
 * framing instead of decoding an incomplete network fragment as a full frame. */
static int decode_stream(int fd,int broker) {
    unsigned char data[8192];size_t used=0,total=0,samples=0;int rate=0,channels=0,eof=0;
    unsigned char tail[8];size_t tail_size=0;
    mp3dec_t decoder;mp3dec_init(&decoder);mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int64_t began=lk_now_ms(),deadline=began+(MAX_SECONDS+5)*1000;
    while(!stopped&&lk_now_ms()<deadline) {
        while(used>=4) {
            if(!hdr_valid(data)){size_t skip=1;while(skip+4<=used&&!hdr_valid(data+skip))skip++;used-=skip;memmove(data,data+skip,used);continue;}
            if(HDR_IS_FREE_FORMAT(data)||HDR_GET_LAYER(data)!=1)return 3;
            int frame=hdr_frame_bytes(data,0)+hdr_padding(data);
            if(frame<4||frame>MAX_FREE_FORMAT_FRAME_SIZE)return 3;
            if(used<(size_t)frame){if(eof)return 3;break;}
            mp3dec_frame_info_t info;int n=mp3dec_decode_frame(&decoder,data,frame,pcm,&info);
            if(info.frame_bytes!=frame)return 3;
            if(n) {
                if(info.channels<1||info.channels>2||info.hz<8000||info.hz>48000)return 3;
                if(!rate){
                    rate=info.hz;channels=info.channels;
                    if(wave_header(rate,channels,0x7ffff000U))return 3;
                    fprintf(stderr,"model-audio: stream first PCM elapsed_ms=%lld rate=%d channels=%d\n",(long long)(lk_now_ms()-began),rate,channels);
                }
                if(rate!=info.hz||channels!=info.channels||(samples+=(size_t)n)>(size_t)rate*MAX_SECONDS)return 3;
                if(write_bytes(pcm,(size_t)n*channels*sizeof(*pcm)))return 3;
            }
            used-=(size_t)frame;memmove(data,data+frame,used);
        }
        if(eof)return samples?0:3;
        struct pollfd p={fd,POLLIN,0};int ready=poll(&p,1,20);
        if(ready<0){if(errno==EINTR)continue;return 3;}
        if(!ready)continue;
        unsigned char incoming[4104];memcpy(incoming,tail,tail_size);
        ssize_t n=read(fd,incoming+tail_size,4096);
        if(n>0){
            size_t amount=tail_size+(size_t)n,keep=broker?(amount<8?amount:8):0;
            size_t audio=amount-keep;
            if(used+audio>sizeof(data)||(total+=audio)>MAX_MP3)return 3;
            memcpy(data+used,incoming,audio);used+=audio;tail_size=keep;
            if(keep)memcpy(tail,incoming+audio,keep);
        }
        else if(!n){
            if(broker){uint32_t code;if(tail_size!=8||memcmp(tail,"LXTE",4))return 3;memcpy(&code,tail+4,4);if(ntohl(code))return 3;}
            eof=1;
        }
        else if(errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK)return 3;
    }
    return stopped?130:3;
}
static int decode_pass(const unsigned char*data,int bytes,int emit,int*rate,int*channels,size_t*samples) {
    mp3dec_t decoder;mp3dec_init(&decoder);*samples=0;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];int at=0;
    while(at<bytes) {
        if(stopped)return -1;
        mp3dec_frame_info_t info;int n=mp3dec_decode_frame(&decoder,data+at,bytes-at,pcm,&info);
        if(info.frame_bytes<=0)break;
        if(n) {
            if(info.channels<1||info.channels>2||info.hz<8000||info.hz>48000)return -1;
            if(!*rate){*rate=info.hz;*channels=info.channels;}
            if(*rate!=info.hz||*channels!=info.channels)return -1;
            *samples+=(size_t)n;
            if(*samples>(size_t)*rate*MAX_SECONDS)return -1;
            if(emit&&write_bytes(pcm,(size_t)n*info.channels*sizeof(*pcm)))return -1;
        }
        at+=info.frame_bytes;
    }
    return *samples?0:-1;
}
static int decode_file(int fd) {
    LX06_STATBUF st;if(LX06_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<4||st.st_size>MAX_MP3)return 3;
    size_t size=(size_t)st.st_size;unsigned char*data=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);
    if(data==MAP_FAILED)return 3;
    int rate=0,channels=0;size_t count=0;int rc=decode_pass(data,(int)size,0,&rate,&channels,&count);
    if(!rc) {
        unsigned char header[44]={0};unsigned length=(unsigned)(count*channels*2);
        memcpy(header,"RIFF",4);little(header+4,length+36,4);memcpy(header+8,"WAVEfmt ",8);
        little(header+16,16,4);little(header+20,1,2);little(header+22,channels,2);little(header+24,rate,4);
        little(header+28,rate*channels*2,4);little(header+32,channels*2,2);little(header+34,16,2);
        memcpy(header+36,"data",4);little(header+40,length,4);
        size_t actual=0;
        rc=write_bytes(header,sizeof(header))||decode_pass(data,(int)size,1,&rate,&channels,&actual)||actual!=count;
    }
    munmap(data,size);return rc?3:0;
}
#include "model-audio-cache.h"
/* One account synthesis request at a time, across installed providers and
 * background greeting warmers. A cache writer lock is insufficient: long
 * sentences skip the cache, and a cache miss formerly submitted a second ubus
 * request whenever another process owned that writer lock. This independent
 * tmpfs directory lease covers only native synthesis/asset acquisition, never
 * speaker playback or stdout backpressure. Official XiaoAI is not locked. */
static int synthesis_lease(int background,int client) {
    if(mkdir(MODEL_TTS_OWNER,0700)&&errno!=EEXIST)return -1;
    int fd=open(MODEL_TTS_OWNER,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    LX06_STATBUF st;
    if(LX06_FSTAT(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=MODEL_CACHE_UID||
       (st.st_mode&07777)!=0700){close(fd);return -1;}
    int64_t began=lk_now_ms();
    while(!stopped) {
        if(client>=0){
            struct pollfd peer={client,0,0};(void)poll(&peer,1,0);
            if(worker_stopping||(peer.revents&(POLLERR|POLLHUP|POLLNVAL))){close(fd);return -3;}
        }
        if(!flock(fd,LOCK_EX|LOCK_NB)) {
            int64_t waited=lk_now_ms()-began;
            if(waited>=20)fprintf(stderr,"model-audio: synthesis lease wait_ms=%lld\n",(long long)waited);
            return fd;
        }
        if(errno!=EWOULDBLOCK&&errno!=EAGAIN&&errno!=EINTR){close(fd);return -1;}
        if(background||lk_now_ms()-began>=MODEL_TTS_WAIT_MS){close(fd);return -2;}
        poll(NULL,0,20);
    }
    close(fd);return -3;
}
static int call_tts(const char*text,char*out,size_t cap,int streaming) {
    char request[4096];size_t n=0;
    const char*start="{\"text\":\"";memcpy(request,start,strlen(start));n=strlen(start);
    for(const unsigned char*p=(const unsigned char*)text;*p;p++) {
        if(*p=='"'||*p=='\\'){request[n++]='\\';request[n++]=*p;}
        else if(*p<32){int z=snprintf(request+n,sizeof(request)-n,"\\u%04x",*p);n+=(size_t)z;}
        else request[n++]=(char)*p;
    }
    snprintf(request+n,sizeof(request)-n,"\",\"caller\":\"%s\",\"vendor\":\"xiaomi\",\"codec\":\"mp3\",\"save\":%d,\"play\":%d}",streaming?"mediaplayer":"lx06-model",!streaming,streaming);
    int p[2];if(pipe2(p,O_CLOEXEC))return -1;pid_t pid=fork();
    if(pid<0){close(p[0]);close(p[1]);return -1;}
    if(!pid) {
        /* The streaming broker keeps this reply reader alive after a service
         * group TERM, too: losing ubus before its URL would orphan a later
         * native FIFO. Only the worker's own bounded timeout sends SIGKILL.
         * Ordinary file synthesis retains its existing session cancellation. */
        signal(SIGTERM,streaming?SIG_IGN:SIG_DFL);signal(SIGINT,streaming?SIG_IGN:SIG_DFL);dup2(p[1],1);
        int null=open("/dev/null",O_WRONLY);if(null>=0)dup2(null,2);close(p[0]);close(p[1]);
        execl(MODEL_UBUS,"ubus","-t","12","call","mibrain","text_to_speech",request,(char*)NULL);_exit(127);
    }
    close(p[1]);fcntl(p[0],F_SETFL,O_NONBLOCK);size_t used=0;int status=0,exited=0,bad=0;
    int64_t end=lk_now_ms()+15000;
    while(!stopped&&!bad&&lk_now_ms()<end) {
        struct pollfd f={p[0],POLLIN,0};poll(&f,1,50);
        char b[1024];ssize_t z;
        while((z=read(p[0],b,sizeof(b)))>0){if(used+(size_t)z>=cap){bad=1;break;}memcpy(out+used,b,(size_t)z);used+=(size_t)z;}
        if(exited&&z==0)break;
        if(!exited&&waitpid(pid,&status,WNOHANG)==pid)exited=1;
    }
    if(!exited){kill(pid,SIGKILL);waitpid(pid,&status,0);}close(p[0]);out[used]=0;
    return !bad&&!stopped&&exited&&WIFEXITED(status)&&!WEXITSTATUS(status)?0:-1;
}
static int path_ok(const char*path) {
    const char*prefix=MODEL_TTS_PREFIX;size_t n=strlen(prefix);
    if(strncmp(path,prefix,n)||strlen(path)>255)return 0;
    for(const unsigned char*p=(const unsigned char*)path+n;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return 0;
    return !strstr(path+n,"..");
}
/* This is not a provider URL fetcher. Only the verified stock loopback FIFO
 * endpoint can be consumed; arbitrary hosts, ports and paths are rejected. */
static int stream_path(const char *url,char *path,size_t cap) {
    char prefix[64];snprintf(prefix,sizeof(prefix),"http://localhost:%d/",MODEL_TTS_HTTP_PORT);
    size_t n=strlen(prefix);if(strncmp(url,prefix,n))return 0;
    const char *asset=url+n;int size=snprintf(path,cap,"%s%s",*asset=='/'?"":"/",asset);
    return size>0&&(size_t)size<cap&&path_ok(path);
}
static void stream_close_inherited(int lease,int pipe_fd) {
    /* session-run/keeper descriptors must not survive in a silent drainer.
     * Its sole authority is the native synthesis flock, never session owner. */
    DIR *dir=opendir("/proc/self/fd");if(!dir)return;
    int listing=dirfd(dir);struct dirent *entry;
    while((entry=readdir(dir))) {
        char *end;long fd=strtol(entry->d_name,&end,10);
        if(!*entry->d_name||*end||fd<3||fd==listing||fd==lease||fd==pipe_fd)continue;
        close((int)fd);
    }
    closedir(dir);close(0);close(1);
    /* call_tts() creates a pipe and dup2s its writer to stdout. Keep stdio
     * descriptors allocated so a new pipe cannot itself reuse fd 1 and have
     * that newly duplicated ubus output closed during normal pipe cleanup. */
    int null=open("/dev/null",O_RDWR);
    if(null>=0){if(null!=0)dup2(null,0);if(null!=1)dup2(null,1);if(null>1)close(null);}
}
static int stream_wait(int fd,short events,int64_t deadline) {
    while(!stopped&&lk_now_ms()<deadline) {
        struct pollfd p={fd,events,0};int ready=poll(&p,1,20);
        if(ready>0)return p.revents&events?0:-1;
        if(ready<0&&errno!=EINTR)return -1;
    }
    return -1;
}
/* A silent, bounded relay owns the native FIFO reader independently of the
 * PCM/player process group. When that group is interrupted its MP3 pipe closes;
 * this relay discards the rest through native EOF so the stock SDK is not left
 * waiting for a FIFO reader. No audio device is opened and no global stop is
 * issued. The compressed spool is anonymous TMP, at most MAX_MP3 bytes. It lets
 * HTTP drain at synthesis speed even if aplay consumes PCM at speech speed.
 * Session-exit subtree cleanup remains an explicit device acceptance gate. */
static int stream_relay(const char *path,int output,int lease) {
    int result=3,sock=-1,spool=-1;size_t received=0,sent=0;int discard=output<0,eof=0;
    int64_t start=lk_now_ms(),http_deadline=start+MODEL_TTS_STREAM_MS;
    sock=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(sock<0)goto done;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(MODEL_TTS_HTTP_PORT)};
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(sock,(struct sockaddr*)&address,sizeof(address))&&errno!=EINPROGRESS)goto done;
    if(stream_wait(sock,POLLOUT,start+1000))goto done;
    int error=0;socklen_t error_size=sizeof(error);
    if(getsockopt(sock,SOL_SOCKET,SO_ERROR,&error,&error_size)||error)goto done;
    char request[512];int count=snprintf(request,sizeof(request),"GET /%s HTTP/1.0\r\nHost: localhost:%d\r\nConnection: close\r\n\r\n",path,MODEL_TTS_HTTP_PORT);
    if(count<=0||(size_t)count>=sizeof(request))goto done;
    size_t offset=0;
    while(offset<(size_t)count) {
        ssize_t n=send(sock,request+offset,(size_t)count-offset,MSG_NOSIGNAL);
        if(n>0){offset+=(size_t)n;continue;}
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)&&!stream_wait(sock,POLLOUT,http_deadline))continue;
        goto done;
    }
    char header[4096];size_t header_size=0;
    /* Read no body byte before validating the bounded header. No redirects,
     * compression, chunked transfer or external credentials are accepted. */
    while(header_size+1<sizeof(header)) {
        if(stream_wait(sock,POLLIN,http_deadline))goto done;
        ssize_t n=recv(sock,header+header_size,1,0);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
        if(n!=1)goto done;
        header_size++;header[header_size]=0;
        if(header_size>=4&&!memcmp(header+header_size-4,"\r\n\r\n",4))break;
    }
    if(header_size+1==sizeof(header)||
       (strncmp(header,"HTTP/1.0 200 ",13)&&strncmp(header,"HTTP/1.1 200 ",13))||
       strcasestr(header,"Transfer-Encoding:")||strcasestr(header,"Content-Encoding:"))goto done;
    char temporary[]="/tmp/lx06-tts-relay-XXXXXX";
    spool=mkstemp(temporary);if(spool<0)goto done;
    unlink(temporary);(void)fcntl(spool,F_SETFD,FD_CLOEXEC);
    if(output>=0)(void)fcntl(output,F_SETFL,O_NONBLOCK);
    while(lk_now_ms()<start+(MAX_SECONDS+5)*1000) {
        if(!eof&&lk_now_ms()>=http_deadline)goto done;
        if(discard)sent=received;
        if(eof&&(discard||sent==received)){result=0;break;}
        struct pollfd fds[2]={{sock,eof?0:POLLIN,0},{output,discard||sent==received?0:POLLOUT,0}};
        int ready=poll(fds,2,20);if(ready<0){if(errno==EINTR)continue;goto done;}
        if(!discard&&(fds[1].revents&(POLLERR|POLLHUP|POLLNVAL))){discard=1;close(output);output=-1;}
        if(!eof&&(fds[0].revents&(POLLIN|POLLHUP|POLLERR))) {
            unsigned char data[4096];ssize_t n=recv(sock,data,sizeof(data),0);
            if(n>0) {
                if(received+(size_t)n>MAX_MP3)goto done;
                if(!discard&&pwrite(spool,data,(size_t)n,(off_t)received)!=n)goto done;
                received+=(size_t)n;
            } else if(!n) {
                eof=1;close(sock);sock=-1;close(lease);lease=-1;
            } else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)goto done;
        }
        if(!discard&&sent<received&&(fds[1].revents&POLLOUT)) {
            unsigned char data[4096];size_t amount=received-sent;if(amount>sizeof(data))amount=sizeof(data);
            ssize_t n=pread(spool,data,amount,(off_t)sent);if(n<=0)goto done;
            ssize_t wrote=write(output,data,(size_t)n);
            if(wrote>0)sent+=(size_t)wrote;
            else if(wrote<0&&errno==EPIPE){discard=1;close(output);output=-1;}
            else if(wrote<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)goto done;
        }
    }
done:
    if(sock>=0)close(sock);
    if(spool>=0)close(spool);
    if(lease>=0)close(lease);
    fprintf(stderr,"model-audio: stream relay bytes=%zu discarded=%d elapsed_ms=%lld status=%d\n",received,discard,(long long)(lk_now_ms()-start),result);
    return result;
}
static void stream_terminal(int output,int code) {
    unsigned char terminal[8]={'L','X','T','E'};uint32_t status=htonl((uint32_t)code);memcpy(terminal+4,&status,4);
    size_t at=0;int64_t deadline=lk_now_ms()+1000;
    while(at<sizeof(terminal)) {
        ssize_t n=send(output,terminal+at,sizeof(terminal)-at,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n>0){at+=(size_t)n;continue;}
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)&&!stream_wait(output,POLLOUT,deadline))continue;
        break;
    }
    close(output);
}
static int stream_request(const char *text,int output,int lease) {
    char response[8192],info[4096],url[512],path[256];xz_json_doc a,b;
    int64_t began=lk_now_ms();int result=3;
    for(int attempt=0;attempt<3;attempt++) {
        int64_t request_start=lk_now_ms();int code=-1;
        fprintf(stderr,"model-audio: stream request attempt=%d text_bytes=%zu\n",attempt+1,strlen(text));
        if(call_tts(text,response,sizeof(response),1))break;
        if(xz_json_parse(&a,response,strlen(response))||xz_json_get_int(&a,0,"code",&code))break;
        int64_t elapsed=lk_now_ms()-request_start;
        fprintf(stderr,"model-audio: stream response attempt=%d code=%d elapsed_ms=%lld\n",attempt+1,code,(long long)elapsed);
        if(code) {
            if(code!=-1||attempt>=2||elapsed>=MODEL_TTS_SLOW_FAILURE_MS)break;
            poll(NULL,0,250);continue;
        }
        if(xz_json_get_string(&a,0,"info",info,sizeof(info))||xz_json_parse(&b,info,strlen(info))||
           xz_json_get_string(&b,0,"path",url,sizeof(url))||!stream_path(url,path,sizeof(path))) {
            fputs("model-audio: invalid loopback synthesis asset\n",stderr);break;
        }
        fprintf(stderr,"model-audio: stream ready %lld ms\n",(long long)(lk_now_ms()-began));
        result=stream_relay(path,output,lease);stream_terminal(output,result);return result;
    }
    if(output>=0)stream_terminal(output,3);
    close(lease);fputs("model-audio: stream synthesis failed; not replayed as file synthesis\n",stderr);
    return result;
}
static int stream_directory(void) {
    if(mkdir(MODEL_TTS_OWNER,0700)&&errno!=EEXIST)return 0;
    int fd=open(MODEL_TTS_OWNER,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;
    LX06_STATBUF st;int good=!LX06_FSTAT(fd,&st)&&S_ISDIR(st.st_mode)&&st.st_uid==MODEL_CACHE_UID&&(st.st_mode&07777)==0700;
    close(fd);return good;
}
static int stream_peer(int fd) {
    struct ucred peer;socklen_t n=sizeof(peer);
    return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)&&n==sizeof(peer)&&peer.uid==MODEL_CACHE_UID;
}
static int broker_bytes(int fd,void *data,size_t size,int writing,int64_t deadline) {
    unsigned char *p=data;
    while(size&&!stopped) {
        ssize_t n=writing?send(fd,p,size,MSG_DONTWAIT|MSG_NOSIGNAL):recv(fd,p,size,MSG_DONTWAIT);
        if(n>0){p+=n;size-=(size_t)n;continue;}
        if(n<0&&errno==EINTR)continue;
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&!stream_wait(fd,writing?POLLOUT:POLLIN,deadline))continue;
        return 0;
    }
    return !size;
}
/* Returns 75 only before any request byte was submitted. The caller may safely
 * fall back to old save:1 synthesis in that case; ambiguous/posted failures must
 * never create a second native request. The broker is launched by procd, outside
 * session-run's subreaper tree, not detached from an audio/session subprocess. */
static int stream_connect(void) {
    LX06_STATBUF st;
    if(!stream_directory()||LX06_LSTAT(MODEL_TTS_SOCKET,&st)||!S_ISSOCK(st.st_mode)||
       st.st_uid!=MODEL_CACHE_UID||(st.st_mode&07777)!=0600)return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(strlen(MODEL_TTS_SOCKET)>=sizeof(address.sun_path)){close(fd);return -1;}
    strcpy(address.sun_path,MODEL_TTS_SOCKET);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))||!stream_peer(fd)){close(fd);return -1;}
    return fd;
}
static int stream_ready(void) {
    int fd=stream_connect();if(fd<0)return 75;
    unsigned char header[8]={'L','X','P','1',0,0,0,0},reply[4];int64_t deadline=lk_now_ms()+1000;
    int good=broker_bytes(fd,header,sizeof(header),1,deadline)&&broker_bytes(fd,reply,sizeof(reply),0,deadline)&&!memcmp(reply,"LXOK",4);
    close(fd);return good?0:75;
}
static int stream_synthesize(const char *text) {
    int fd=stream_connect();if(fd<0)return 75;
    unsigned char header[8]={'L','X','T','1'};uint32_t size=htonl((uint32_t)strlen(text));memcpy(header+4,&size,4);
    int64_t deadline=lk_now_ms()+5000;
    if(!broker_bytes(fd,header,sizeof(header),1,deadline)||!broker_bytes(fd,(void*)text,strlen(text),1,deadline)){close(fd);return stopped?130:3;}
    int rc=decode_stream(fd,1);close(fd);return stopped?130:rc;
}
static int stream_text_valid(const unsigned char *text,size_t n) {
    for(size_t i=0;i<n;) {
        unsigned c=text[i++],need,cp,min;
        if(c<128){if(!c)return 0;continue;}
        if(c>=0xc2&&c<=0xdf){need=1;cp=c&31;min=0x80;}
        else if(c>=0xe0&&c<=0xef){need=2;cp=c&15;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){need=3;cp=c&7;min=0x10000;}
        else return 0;
        if(i+need>n)return 0;
        while(need--){unsigned next=text[i++];if((next&0xc0)!=0x80)return 0;cp=(cp<<6)|(next&63);}
        if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
    }
    return 1;
}
static void stream_client(int client) {
    stopped=0;
    worker_stopping=0;struct sigaction pending={0};pending.sa_handler=stop_pending;
    sigemptyset(&pending.sa_mask);sigaction(SIGUSR1,&pending,NULL);
    /* Some procd variants terminate the service process group. A worker may
     * already own a native URL/FIFO, so group TERM/INT must cancel only queued
     * work, just like the broker's own graceful-stop notification. The parent
     * owns the final bounded KILL after its 45s drain grace. */
    sigaction(SIGTERM,&pending,NULL);sigaction(SIGINT,&pending,NULL);
    if(!stream_peer(client)){close(client);_exit(3);}
    unsigned char header[8];char text[541];int64_t deadline=lk_now_ms()+2000;
    if(!broker_bytes(client,header,sizeof(header),0,deadline)){close(client);_exit(3);}
    uint32_t size;memcpy(&size,header+4,4);size=ntohl(size);
    if(!memcmp(header,"LXP1",4)&&!size){char reply[]="LXOK";(void)broker_bytes(client,reply,4,1,deadline);close(client);_exit(0);}
    if(memcmp(header,"LXT1",4)){close(client);_exit(3);}
    if(!size||size>=sizeof(text)||!broker_bytes(client,text,size,0,deadline)||!stream_text_valid((unsigned char*)text,size)){close(client);_exit(3);}
    text[size]=0;int lease=synthesis_lease(0,client);
    if(lease<0){close(client);_exit(3);}
    /* procd owns this process tree. No client/session descriptors survive;
     * closing a client at any request stage only turns this worker into a
     * bounded silent drain. Its native request remains correlated to its URL. */
    stream_close_inherited(lease,client);_exit(stream_request(text,client,lease));
}
static void stream_reap(pid_t *children) {
    for(int i=0;i<4;i++)if(children[i]&&waitpid(children[i],NULL,WNOHANG)==children[i])children[i]=0;
}
static int stream_serve(void) {
    if(geteuid()!=MODEL_CACHE_UID||!stream_directory())return 3;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(strlen(MODEL_TTS_SOCKET)>=sizeof(address.sun_path))return 3;
    strcpy(address.sun_path,MODEL_TTS_SOCKET);
    LX06_STATBUF before,after;
    if(!LX06_LSTAT(MODEL_TTS_SOCKET,&before)) {
        if(!S_ISSOCK(before.st_mode)||before.st_uid!=MODEL_CACHE_UID||(before.st_mode&07777)!=0600)return 3;
        int check=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(check<0)return 3;
        int connected=connect(check,(struct sockaddr*)&address,sizeof(address)),error=errno;close(check);
        if(!connected||error!=ECONNREFUSED)return 75;
        if(LX06_LSTAT(MODEL_TTS_SOCKET,&after)||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||unlink(MODEL_TTS_SOCKET))return 3;
    } else if(errno!=ENOENT)return 3;
    int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(server<0)return 3;
    mode_t mask=umask(0177);int bound=bind(server,(struct sockaddr*)&address,sizeof(address));umask(mask);
    if(bound||listen(server,4)||LX06_LSTAT(MODEL_TTS_SOCKET,&before)){close(server);return 3;}
    pid_t children[4]={0};fputs("model-audio: stream broker ready workers=4\n",stderr);
    while(!stopped) {
        stream_reap(children);int slot=-1;for(int i=0;i<4;i++)if(!children[i]){slot=i;break;}
        struct pollfd p={server,slot<0?0:POLLIN,0};int ready=poll(&p,1,50);
        if(ready<0){if(errno==EINTR)continue;break;}
        if(slot<0||ready<=0)continue;
        int client=accept4(server,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);if(client<0)continue;
        pid_t pid=fork();if(!pid){close(server);stream_client(client);}
        close(client);if(pid>0)children[slot]=pid;
    }
    close(server);
    if(!LX06_LSTAT(MODEL_TTS_SOCKET,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino)unlink(MODEL_TTS_SOCKET);
    /* Pending clients must not start new synthesis during service shutdown.
     * Posted workers ignore this flag and retain their original URL/EOF duty. */
    for(int i=0;i<4;i++)if(children[i])kill(children[i],SIGUSR1);
    /* Stop accepting immediately, but allow posted native work to reach EOF.
     * Native URL+HTTP timeouts fit inside this bounded service shutdown grace.
     * A slow PCM consumer can be killed after native EOF without affecting SDK. */
    int64_t deadline=lk_now_ms()+45000;
    while(lk_now_ms()<deadline) {
        stream_reap(children);int left=0;for(int i=0;i<4;i++)left+=children[i]!=0;
        if(!left)return 0;
        poll(NULL,0,20);
    }
    for(int i=0;i<4;i++)if(children[i])kill(children[i],SIGKILL);
    for(int i=0;i<4;i++)if(children[i])while(waitpid(children[i],NULL,0)<0&&errno==EINTR){}
    return 0;
}
int main(int argc,char**argv) {
    signal(SIGPIPE,SIG_IGN);struct sigaction action={0};action.sa_handler=stop;
    sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);
    ready_open();
    if(argc==2&&!strcmp(argv[1],"--serve-stream"))return stream_serve();
    if(argc==2&&!strcmp(argv[1],"--stream-ready"))return stream_ready();
    if(argc==3&&!strcmp(argv[1],"--decode")) {
        int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 2;
        int rc=decode_file(fd);close(fd);return rc;
    }
    if(argc==2&&!strcmp(argv[1],"--decode-stream"))return decode_stream(0,0);
    int streaming=argc==2&&!strcmp(argv[1],"--stream-synthesize");
    int fresh=argc==2&&!strcmp(argv[1],"--synthesize-fresh");
    int force=argc==2&&!strcmp(argv[1],"--regenerate-cache");
    int status=argc==2&&!strcmp(argv[1],"--cache-status");
    int warm=force||status||(argc==2&&!strcmp(argv[1],"--warm-cache"));
    if(argc!=2||(!warm&&!streaming&&!fresh&&strcmp(argv[1],"--synthesize")))return 2;
    /* A bounded sentence, not a whole model response. stdin keeps spoken text
     * out of the helper's argv; native ubus itself requires its JSON argv. */
    int64_t synth_start=lk_now_ms();
    char text[541];size_t length=fread(text,1,sizeof(text)-1,stdin);int extra=fgetc(stdin);
    if(!length||extra!=EOF||ferror(stdin)||memchr(text,0,length))return warm?3:2;
    text[length]=0;
    if(warm&&length>AUDIO_CACHE_TEXT){if(status)puts("{\"status\":\"unsupported\"}");return 3;}
    struct audio_cache cache={.dir=-1,.temp=-1};
    if(!fresh)cache_open(&cache,text,length,warm);
    if(status){
        int hit=cache_exists(&cache),rc=hit?0:cache.dir<0?3:cache_lock(&cache)?4:75;
        printf("{\"status\":\"%s\",\"cache_key\":\"%.*s\"}\n",
            hit?"ready":rc==4?"missing":rc==75?"busy":"unavailable",64,cache.name+(cache.greeting?2:0));
        cache_close(&cache);return rc;
    }
    int cached=force?0:cache_read(&cache,warm);
    if(cached){cache_close(&cache);return stopped?130:cached<0?3:0;}
    if(streaming){
        cache_close(&cache);int rc=stream_synthesize(text);if(rc!=75)return rc;
        fputs("model-audio: stream broker unavailable before request; file synthesis fallback\n",stderr);
        streaming=0;cache_open(&cache,text,length,0);
    }
    /* Explicit preparation reports success only when retained. A cache hit
     * needs no writer lock; regeneration leaves the old WAV available while
     * its replacement is produced. Normal long replies keep their fallback. */
    if(warm&&cache.dir<0)return 3;
    int lease=synthesis_lease(warm,-1);
    if(lease<0){
        if(lease!=-3)fprintf(stderr,"model-audio: synthesis lease %s\n",lease==-2?"busy":"unavailable");
        cache_close(&cache);return stopped?130:lease==-2?75:3;
    }
    /* A warmer may have committed the exact phrase while we waited. Keep its
     * open inode, then unlock before output so even a blocked caller cannot
     * hold the stock synthesis slot. Force regeneration deliberately bypasses
     * this second cache lookup as well. */
    int cache_fd=force?-1:cache_hit_fd(&cache);
    if(cache_fd>=0){
        close(lease);fputs("model-audio: short phrase cache hit after lease\n",stderr);
        int cached_rc=warm?0:cache_copy(cache_fd);close(cache_fd);cache_close(&cache);
        return stopped?130:cached_rc;
    }
    if(!streaming&&cache.dir>=0&&!cache_lock(&cache)){cache_close(&cache);if(warm)return 75;}
    if(!streaming&&cache.dir>=0&&!cache_prepare(&cache)){cache_close(&cache);if(warm)return 3;}
    char response[8192],info[4096],path[512];time_t begin=time(NULL);
    xz_json_doc a,b;int code=-1;
    for(int attempt=0;;attempt++) {
        if(stopped){cache_close(&cache);return 130;}
        int64_t request_start=lk_now_ms();
        fprintf(stderr,"model-audio: request attempt=%d text_bytes=%zu\n",attempt+1,length);
        if(call_tts(text,response,sizeof(response),streaming)){fprintf(stderr,"model-audio: synthesis transport failed elapsed_ms=%lld\n",(long long)(lk_now_ms()-request_start));cache_close(&cache);return stopped?130:3;}
        if(xz_json_parse(&a,response,strlen(response))||xz_json_get_int(&a,0,"code",&code)){fputs("model-audio: malformed synthesis status\n",stderr);cache_close(&cache);return 3;}
        fprintf(stderr,"model-audio: response attempt=%d code=%d elapsed_ms=%lld\n",attempt+1,code,(long long)(lk_now_ms()-request_start));
        if(!code)break;
        /* Stock -1 also covers mibrain_service's six-second receive timeout,
         * not just an immediate busy slot. Such a timed-out operation may
         * still be in the SDK; blindly submitting it again creates duplicate
         * work and another six-second stall. Only a quick explicit -1 can
         * use the existing two bounded retries. The owning core reports a
         * failed operation and resumes listening with its ASR still alive. */
        int slow_failure=code==-1&&lk_now_ms()-request_start>=MODEL_TTS_SLOW_FAILURE_MS;
        if(code!=-1||attempt>=2||slow_failure){
            fprintf(stderr,"model-audio: synthesis status=%d%s\n",code,slow_failure?"; stock receive timeout, not resubmitted":"");
            cache_close(&cache);return 3;
        }
        fputs("model-audio: stock synthesis status=-1; bounded retry\n",stderr);
        poll(NULL,0,250);
    }
    if(xz_json_get_string(&a,0,"info",info,sizeof(info))||xz_json_parse(&b,info,strlen(info))||
       xz_json_get_string(&b,0,"path",path,sizeof(path))){fputs("model-audio: invalid synthesis asset\n",stderr);cache_close(&cache);return 3;}
    if(!path_ok(path)){fputs("model-audio: invalid synthesis asset\n",stderr);cache_close(&cache);return 3;}
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){fprintf(stderr,"model-audio: asset open errno=%d\n",errno);cache_close(&cache);return 3;}
    LX06_STATBUF before,after;int owned=!LX06_FSTAT(fd,&before)&&S_ISREG(before.st_mode)&&before.st_mtime>=begin;
    close(lease);
    fprintf(stderr,"model-audio: asset ready %lld ms\n",(long long)(lk_now_ms()-synth_start));
    int64_t decode_start=lk_now_ms();int rc;
    if(cache.temp>=0) {
        output_fd=cache.temp;output_limit=AUDIO_CACHE_ENTRY;output_size=0;output_overflow=0;
        rc=stopped?130:decode_file(fd);
        output_fd=1;output_limit=0;output_size=0;
        if(!rc&&cache_commit(&cache)) {
            fprintf(stderr,"model-audio: short phrase cached\n");
            rc=warm?0:cache_copy(cache.temp);
        } else if(!warm&&!stopped) {
            /* Cache limits/I/O are optional; normal synthesis retains its old
             * duration/MP3 bounds and succeeds even for a large short phrase. */
            rc=decode_file(fd);
        } else if(warm)rc=3;
    } else rc=stopped?130:decode_file(fd);
    fprintf(stderr,"model-audio: decode/output elapsed_ms=%lld status=%d\n",(long long)(lk_now_ms()-decode_start),rc);
    if(rc&&!stopped)fputs("model-audio: invalid or oversized audio\n",stderr);
    /* Clean only the exact newly returned inode; never glob /tmp/tts or touch
     * playback belonging to stock XiaoAI/a newer session. */
    if(owned&&!LX06_LSTAT(path,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino)unlink(path);
    close(fd);cache_close(&cache);return stopped?130:rc;
}
