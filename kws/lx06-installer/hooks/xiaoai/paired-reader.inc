/* Both streams originate in the same credential-checked stock audio process. */
static int paired_socket(const char *path,const char *lockpath,int *lock) {
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s",path);
    *lock=open(lockpath,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(*lock<0 || flock(*lock,LOCK_EX|LOCK_NB)) return -1;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(fd<0) return -1;
    struct stat st;
    if(lstat(path,&st)==0) {
        if(!S_ISSOCK(st.st_mode) || st.st_uid!=geteuid() ||
           !connect(fd,(struct sockaddr *)&addr,sizeof(addr)) || errno!=ECONNREFUSED ||
           unlink(path)) {close(fd);return -1;}
    } else if(errno!=ENOENT) {close(fd);return -1;}
    int yes=1, buffer=65536;
    if(setsockopt(fd,SOL_SOCKET,SO_PASSCRED,&yes,sizeof(yes)) ||
       setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer)) ||
       bind(fd,(struct sockaddr *)&addr,sizeof(addr))) {close(fd);return -1;}
    return fd;
}
static int read_paired(void) {
#ifndef LX06_PAIRED_MIC_DIR
#define LX06_PAIRED_MIC_DIR "/tmp/lx06-mic"
#endif
    const char *paths[]={LX06_PAIRED_MIC_DIR "/kws.kt1",LX06_PAIRED_MIC_DIR "/kws.ke2"};
    const char *locks[]={LX06_PAIRED_MIC_DIR "/kws.lock",LX06_PAIRED_MIC_DIR "/kws.ke2.lock"};
    struct pollfd p[3]={{.fd=-1,.events=POLLIN},{.fd=-1,.events=POLLIN},{.fd=1}};
    /* The detector may synchronously await an ASR response for 800 ms.
     * Both producer streams total 71,000 B/s. Preserve mux order, including
     * original timestamps, for up to 1.84s of backpressure. */
    static unsigned char queue[131072];
    size_t head=0,used=0;
    int lock[2]={-1,-1},rc=3;pid_t producer=0;
    mkdir(LX06_PAIRED_MIC_DIR,0755);
    for(int i=0;i<2;i++) if((p[i].fd=paired_socket(paths[i],locks[i],&lock[i]))<0) goto done;
    int flags=fcntl(1,F_GETFL);
    if(flags<0 || fcntl(1,F_SETFL,flags|O_NONBLOCK)) goto done;
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    long long last_stock=now_ms();
    while(!stopped) {
        p[2].events=used ? POLLOUT : 0;
        int ready=poll(p,3,1000);
        if(ready<0 && errno==EINTR) continue;
        if(ready<0 || now_ms()-last_stock>3000) break;
        if(p[2].revents&(POLLERR|POLLHUP|POLLNVAL)) break;
        if(p[2].revents&POLLOUT) {
            size_t available=sizeof(queue)-head;
            if(available>used) available=used;
            ssize_t n=write(1,queue+head,available);
            if(n>0) {head=(head+(size_t)n)%sizeof(queue);used-=(size_t)n;}
            else if(!n || (errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)) break;
        }
        for(int i=1;i>=0;i--) {
            if(p[i].revents & (POLLERR|POLLHUP|POLLNVAL)) goto done;
            if(!(p[i].revents&POLLIN)) continue;
            unsigned char packet[LX06_WAKE_FRAME_BYTES];
            size_t n=i ? sizeof(packet) : 364;
            if(kws_receive(p[i].fd,packet,n,geteuid(),&producer) ||
               memcmp(packet,i ? "KWE2" : "KWT1",4)) goto done;
            if(!i) {
                /* This clock only supervises reader liveness. Retain the
                 * producer's KWT1 timestamp, including after pipe stalls. */
                last_stock=now_ms();
            }
            if(n>sizeof(queue)-used) {
                fputs("PAIR_STREAM reader-backlog-full\n",stderr);goto done;
            }
            size_t tail=(head+used)%sizeof(queue);
            size_t first=sizeof(queue)-tail;
            if(first>n) first=n;
            memcpy(queue+tail,packet,first);
            memcpy(queue,packet+first,n-first);
            used+=n;
        }
    }
    rc=stopped ? 0 : 3;
done:
    for(int i=0;i<2;i++) {
        if(p[i].fd>=0) {unlink(paths[i]);close(p[i].fd);}
        if(lock[i]>=0) close(lock[i]);
    }
    return rc;
}
