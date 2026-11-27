/* Root-only tmpfs cache for exact short TTS texts.
 * A reader never takes the writer lock, so a manual regeneration cannot block
 * a valid greeting already being used. Writers stage a bounded replacement;
 * the old entry survives synthesis/decode failures. Official tone changes are
 * deliberately manual: --regenerate-cache replaces the same text's greeting
 * using the tone currently selected in stock XiaoAI. No voice probing occurs.
 * It never stores account
 * data or MP3, only a completely validated PCM WAV from decode_file(). Each
 * writer holds a nonblocking directory flock: no wake waits for a warmer.
 * Cache loss/failure always falls back to the uncached synthesis path.
 */
#ifndef LX06_MODEL_AUDIO_CACHE_H
#define LX06_MODEL_AUDIO_CACHE_H
#include <dirent.h>
#include <sys/file.h>
#ifndef MODEL_AUDIO_CACHE
#define MODEL_AUDIO_CACHE "/tmp/lx06-model-tts-cache"
#endif
#ifndef MODEL_CACHE_UID
#define MODEL_CACHE_UID 0
#endif
#define AUDIO_CACHE_TEXT 240
#define AUDIO_CACHE_ANSWER_TEXT 96
#define AUDIO_CACHE_ENTRY (96*1024)
#define AUDIO_CACHE_TOTAL (256*1024)
#define AUDIO_CACHE_COUNT 8
extern unsigned char *SHA256(const unsigned char*,size_t,unsigned char*);
struct audio_cache {int dir,temp,locked,greeting,answer_cache;char name[71],pending[48];};
static unsigned cache_le(const unsigned char *p,int n){unsigned v=0;for(int i=n-1;i>=0;i--)v=(v<<8)|p[i];return v;}
static int cache_owned(int fd,LX06_STATBUF *st) {
    return !LX06_FSTAT(fd,st)&&S_ISREG(st->st_mode)&&st->st_uid==MODEL_CACHE_UID&&
        (st->st_mode&07777)==0600&&st->st_nlink==1&&st->st_size>=0&&st->st_size<=AUDIO_CACHE_ENTRY;
}
static int cache_wave(int fd) {
    LX06_STATBUF st;unsigned char h[44];if(!cache_owned(fd,&st)||st.st_size<44||pread(fd,h,44,0)!=44)return 0;
    unsigned channels=cache_le(h+22,2),rate=cache_le(h+24,4),size=cache_le(h+40,4);
    return !memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVEfmt ",8)&&!memcmp(h+36,"data",4)&&
        cache_le(h+16,4)==16&&cache_le(h+20,2)==1&&channels>=1&&channels<=2&&rate>=8000&&rate<=48000&&
        cache_le(h+28,4)==rate*channels*2&&cache_le(h+32,2)==channels*2&&cache_le(h+34,2)==16&&
        size>0&&!(size%(channels*2))&&size+44==(unsigned)st.st_size&&cache_le(h+4,4)==size+36;
}
static int cache_copy(int fd) {
    char buf[4096];off_t at=0;
    for(;;){ssize_t n=pread(fd,buf,sizeof(buf),at);if(n<0&&errno==EINTR)continue;if(n<0)return 3;if(!n)return 0;if(write_bytes(buf,(size_t)n))return 3;at+=n;}
}
static int cache_name(const char *s) {
    if(!strncmp(s,"g-",2))s+=2;
    if(strlen(s)!=68||strcmp(s+64,".wav"))return 0;
    for(int i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;
    return 1;
}
static int cache_partial(const char *s) {
    if(strncmp(s,".part-",6)||!s[6]||strlen(s)>47)return 0;
    for(const char *p=s+6;*p;p++)if(*p<'0'||*p>'9')return 0;
    return 1;
}
static void cache_close(struct audio_cache *c) {
    if(c->temp>=0){close(c->temp);c->temp=-1;}
    if(c->dir>=0){if(*c->pending)unlinkat(c->dir,c->pending,0);close(c->dir);c->dir=-1;}
}
static void cache_open(struct audio_cache *c,const char *text,size_t n,int greeting) {
    memset(c,0,sizeof(*c));c->dir=c->temp=-1;
    c->greeting=greeting;c->answer_cache=n<=AUDIO_CACHE_ANSWER_TEXT;
    if(n>AUDIO_CACHE_TEXT||geteuid()!=MODEL_CACHE_UID)return;
    if(mkdir(MODEL_AUDIO_CACHE,0700)&&errno!=EEXIST)return;
    int fd=open(MODEL_AUDIO_CACHE,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return;
    LX06_STATBUF st;
    if(LX06_FSTAT(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=MODEL_CACHE_UID||(st.st_mode&07777)!=0700){close(fd);return;}
    c->dir=fd;
    unsigned char digest[32];SHA256((const unsigned char*)text,n,digest);
    char *name=c->name;if(greeting){memcpy(name,"g-",2);name+=2;}
    for(int i=0;i<32;i++)snprintf(name+i*2,3,"%02x",digest[i]);
    strcpy(name+64,".wav");
}
static int cache_lock(struct audio_cache *c) {
    if(c->dir<0)return 0;
    if(c->locked)return 1;
    if(flock(c->dir,LOCK_EX|LOCK_NB))return 0;
    c->locked=1;return 1;
}
static int cache_exists(struct audio_cache *c) {
    if(c->dir<0)return 0;
    int fd=openat(c->dir,c->name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return 0;
    int good=cache_wave(fd);close(fd);return good;
}
/* Return a verified open inode. A foreground caller can release the synthesis
 * lease before copying a cache hit to a backpressured player pipe. */
static int cache_hit_fd(struct audio_cache *c) {
    if(c->dir<0)return -1;
    char greeting[71];int fd=-1;
    if(!c->greeting){
        memcpy(greeting,"g-",2);memcpy(greeting+2,c->name,69);
        fd=openat(c->dir,greeting,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd>=0&&!cache_wave(fd)){close(fd);fd=-1;}
    }
    if(fd<0)fd=openat(c->dir,c->name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return -1;
    if(!cache_wave(fd)){close(fd);return -1;}
    return fd;
}
static int cache_read(struct audio_cache *c,int warm) {
    int fd=cache_hit_fd(c);if(fd<0)return 0;
    fprintf(stderr,"model-audio: short phrase cache hit\n");int rc=warm?0:cache_copy(fd);
    close(fd);return rc?-1:1;
}
static int cache_prepare(struct audio_cache *c) {
    /* A long configured greeting can be read/prepared, but ordinary answers
     * retain the old 96-byte write threshold and decoding cost. */
    if(!c->locked||(!c->greeting&&!c->answer_cache))return 0;
    /* Evict oldest valid own entries until even a maximum-size temporary WAV
     * fits. Unknown files/symlinks disable writes instead of following them. */
    for(int tries=0;tries<32;tries++) {
        int scan=openat(c->dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scan<0)return 0;
        DIR *dir=fdopendir(scan);if(!dir){close(scan);return 0;}
        unsigned count=0,total=0,seen=0,target=0;char oldest[71]="";time_t stamp=0;int bad=0,oldest_greeting=0;struct dirent *e;
        while((e=readdir(dir))) {
            const char *name=e->d_name;if(!strcmp(name,".")||!strcmp(name,".."))continue;
            if(++seen>64){bad=1;break;}
            if(cache_partial(name)) {
                int old=openat(c->dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);LX06_STATBUF partial;
                int own=old>=0&&!LX06_FSTAT(old,&partial)&&S_ISREG(partial.st_mode)&&partial.st_uid==MODEL_CACHE_UID&&
                    (partial.st_mode&07777)==0600&&partial.st_nlink==1&&partial.st_size>=0&&partial.st_size<=AUDIO_CACHE_ENTRY;
                if(old>=0)close(old);
                if(!own||unlinkat(c->dir,name,0)){bad=1;break;}
                continue;
            }
            if(!cache_name(name)){bad=1;break;}
            int fd=openat(c->dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);LX06_STATBUF st;
            if(fd<0||!cache_owned(fd,&st)){if(fd>=0)close(fd);bad=1;break;}
            close(fd);count++;total+=(unsigned)st.st_size;
            if(!strcmp(name,c->name))target=1;
            else {
                int greeting=!strncmp(name,"g-",2);
                /* Ordinary answers cannot evict a prepared greeting. A new
                 * greeting first reclaims ordinary entries, then oldest
                 * greetings, always within the same count/space bounds. */
                if(greeting&&!c->greeting)continue;
                if(!*oldest||greeting<oldest_greeting||(greeting==oldest_greeting&&st.st_mtime<stamp)){
                    strcpy(oldest,name);stamp=st.st_mtime;oldest_greeting=greeting;
                }
            }
        }
        closedir(dir);if(bad)return 0;
        /* Reserve the full stage without ever evicting the entry being
         * regenerated. Other greetings follow the existing bounded eviction
         * policy; even a failed force operation leaves this old key usable. */
        if(count-target<AUDIO_CACHE_COUNT&&total<=AUDIO_CACHE_TOTAL-AUDIO_CACHE_ENTRY)break;
        if(!*oldest||unlinkat(c->dir,oldest,0))return 0;
        if(tries==31)return 0;
    }
    snprintf(c->pending,sizeof(c->pending),".part-%ld",(long)getpid());
    c->temp=openat(c->dir,c->pending,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(c->temp<0){c->pending[0]=0;return 0;}
    return 1;
}
static int cache_commit(struct audio_cache *c) {
    if(c->temp<0||!cache_wave(c->temp)||stopped||renameat(c->dir,c->pending,c->dir,c->name))return 0;
    c->pending[0]=0;return 1;
}
#endif
