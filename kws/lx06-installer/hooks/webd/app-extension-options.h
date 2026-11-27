/* Successful dynamic catalogues are bounded TMP data, shared by the Web API,
 * runner and settings helper. Exact declaration bytes bind a cache to its
 * script schema; replacing a script cannot silently reuse an old allowlist. */
#ifndef LX06_APP_EXTENSION_OPTIONS_H
#define LX06_APP_EXTENSION_OPTIONS_H
#include <sys/stat.h>
#include <fcntl.h>
#include "../../runtime/common/script-settings-store.h"
/* Same flat record owner used by show/save/run and retained preparation.
 * Values remain JSON-encoded strings; no new disk format or namespace. */
static int ext_settings_decode_ignoring(struct ext_schema*s,const char*raw,unsigned restoring) {
    xz_json_doc d;if(strlen(raw)>SETTINGS_CAP||xz_json_parse(&d,raw,strlen(raw))||d.tokens[0].type!=XZ_JSON_OBJECT)return -1;
    s->saved_mask=0;
    for(int k=d.tokens[0].first_child;k>=0;) {
        char key[33],v[SETTINGS_CAP+1];int t=d.tokens[k].next_sibling;
        if(t<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||!ext_text(&d,0,key,v,sizeof(v),0))return -1;
        for(int p=d.tokens[0].first_child;p!=k;p=d.tokens[d.tokens[p].next_sibling].next_sibling)if(ext_raw_eq(&d,p,key))return -1;
        int f=ext_field_index(s,key);if(f>=0){if(strlen(v)>2048)return -1;strcpy(s->saved_values[f],v);s->saved_mask|=1U<<f;}
        k=d.tokens[t].next_sibling;
    }
    for(int i=0;i<s->nf;i++) {
        char value[2049];if(!(restoring&(1U<<i))&&ext_effective_value(s,i,value)&&!ext_value(s,i,value))return -1;
    }
    return 0;
}
static int ext_settings_decode(struct ext_schema*s,const char*raw) {return ext_settings_decode_ignoring(s,raw,0);}
static int ext_settings_load(struct ext_schema*s,int dir,const char*name,char raw[SETTINGS_CAP+1]) {
    if(dir<0&&errno!=ENOENT)return -1;
    strcpy(raw,"{}");int rc=dir<0?0:settings_read(dir,name,raw);
    return rc<0||ext_settings_decode(s,raw)?-1:rc;
}
struct ext_settings_view {
    struct ext_schema *schema;
    char raw[SETTINGS_CAP+1],script[512],name[65];
    SETTINGS_STAT script_stat,record_stat,dir_stat;
    int dir,present,invalid,owner_lock; /* owned dup of original lock, -1 for ordinary reads */
};
static int ext_settings_identity(const SETTINGS_STAT*a,const SETTINGS_STAT*b) {
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static void ext_settings_close(struct ext_settings_view*v) {
    if(v->schema){memset(v->schema,0,sizeof(*v->schema));free(v->schema);}
    if(v->dir>=0)close(v->dir);
    if(v->schema&&v->owner_lock>=0)close(v->owner_lock);
    memset(v,0,sizeof(*v));v->dir=v->owner_lock=-1;
}
static int ext_settings_open_impl(struct ext_settings_view*v,const char*script,int owner_lock) {
    memset(v,0,sizeof(*v));v->dir=v->owner_lock=-1;
    const char*name=strrchr(script,'/');name=name?name+1:script;
    if(!settings_name(name)||strlen(script)>=sizeof(v->script))return -1;
    strcpy(v->name,name);strcpy(v->script,script);v->schema=calloc(1,sizeof(*v->schema));
    int f=open(script,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);int ok=f>=0&&v->schema&&
        !SETTINGS_FSTAT(f,&v->script_stat)&&S_ISREG(v->script_stat.st_mode)&&
        v->script_stat.st_uid==getuid()&&!(v->script_stat.st_mode&0022)&&ext_read(f,v->schema)==1;
    if(f>=0)close(f);
    if(!ok)goto bad;
    v->dir=settings_dir(0);
    if(v->dir<0) {
        if(owner_lock>=0||errno!=ENOENT||ext_settings_load(v->schema,-1,name,v->raw)<0)goto bad;
        v->present=0;return 0;
    }
    int borrowed=owner_lock>=0,lock=owner_lock;
    if(borrowed) {
        lock=fcntl(owner_lock,F_DUPFD_CLOEXEC,0);if(lock<0)goto bad;v->owner_lock=lock;
        SETTINGS_STAT held,named;int flags=fcntl(lock,F_GETFL);
        if(flags<0||(flags&O_ACCMODE)==O_RDONLY||SETTINGS_FSTAT(lock,&held)||
           SETTINGS_FSTATAT(v->dir,".lock",&named,AT_SYMLINK_NOFOLLOW)||!S_ISREG(held.st_mode)||
           held.st_uid!=getuid()||held.st_nlink!=1||(held.st_mode&0777)!=0600||
           held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||flock(lock,LOCK_EX|LOCK_NB))goto bad;
    } else {
        lock=openat(v->dir,".lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if((lock<0&&errno!=ENOENT)||(lock>=0&&flock(lock,LOCK_SH|LOCK_NB))){if(lock>=0)close(lock);goto bad;}
    }
    v->present=ext_settings_load(v->schema,v->dir,name,v->raw);
    char file[80];snprintf(file,sizeof(file),"%s.json",name);
    ok=v->present>=0&&(lock>=0||!v->present)&&!SETTINGS_FSTAT(v->dir,&v->dir_stat)&&
        (!v->present||!SETTINGS_FSTATAT(v->dir,file,&v->record_stat,AT_SYMLINK_NOFOLLOW));
    if(!borrowed&&lock>=0)close(lock);
    if(ok)return 0;
bad:ext_settings_close(v);return -1;
}
static int ext_settings_open(struct ext_settings_view*v,const char*script) {
    return ext_settings_open_impl(v,script,-1);
}
/* Latched invalidation: an edit/delete/reorder, schema replacement or store
 * replacement stops this request, even if a later caller restores old bytes. */
static int ext_settings_current(struct ext_settings_view*v) {
    if(!v||!v->schema||v->invalid)return 0;
    struct ext_settings_view*now=calloc(1,sizeof(*now));int ok=now&&!ext_settings_open_impl(now,v->script,v->owner_lock)&&
        now->present==v->present&&ext_settings_identity(&now->script_stat,&v->script_stat)&&
        now->dir_stat.st_ino==v->dir_stat.st_ino&&now->dir_stat.st_dev==v->dir_stat.st_dev&&
        (!v->present||ext_settings_identity(&now->record_stat,&v->record_stat))&&
        !strcmp(now->raw,v->raw)&&!strcmp(now->schema->json,v->schema->json);
    if(now){ext_settings_close(now);free(now);}if(!ok)v->invalid=1;return ok;
}
/* Attach the caller's ORIGINAL retained writer lock to an existing snapshot.
 * The same source/store/record/schema are re-read under that lock. A mismatch
 * latches invalidation; no cached-current shortcut or lock downgrade. */
static int ext_settings_bind_owner(struct ext_settings_view*v,int dir,int lock) {
    SETTINGS_STAT current;
    if(!v||!v->schema||v->invalid||dir<0||lock<0||v->owner_lock>=0||
       SETTINGS_FSTAT(dir,&current)||current.st_dev!=v->dir_stat.st_dev||current.st_ino!=v->dir_stat.st_ino)return -1;
    int retained=fcntl(lock,F_DUPFD_CLOEXEC,0);if(retained<0)return -1;
    v->owner_lock=retained;
    if(!ext_settings_current(v)){close(v->owner_lock);v->owner_lock=-1;return -1;}
    return 0;
}
#ifndef EXT_OPTIONS_ROOT
#define EXT_OPTIONS_ROOT "/tmp/lx06-extensions"
#endif
#ifdef SETTINGS_STAT
#define EXT_OPTIONS_STAT SETTINGS_STAT
#define EXT_OPTIONS_FSTAT SETTINGS_FSTAT
#else
#define EXT_OPTIONS_STAT LX06_STATBUF
#define EXT_OPTIONS_FSTAT LX06_FSTAT
#endif
#define EXT_OPTIONS_CACHE_CAP (EXT_CAP+2*EXT_EVENT_CAP+64)
static int ext_has_dynamic(const struct ext_schema*s) {
    for(int i=0;i<s->nf;i++)if(s->fields[i].dynamic)return 1;
    return 0;
}
static int ext_event_has(const char*event,const char*key) {
    xz_json_doc d;
    return !xz_json_parse(&d,event,strlen(event))&&xz_json_object_get(&d,0,key)>=0;
}
/* Transient structured payloads are not retained by a neighbouring dynamic
 * selector. Preserve the original catalogue/cache and its other small values. */
static int ext_cache_transport_free(const struct ext_schema*s,const char*event) {
    xz_json_doc d;if(xz_json_parse(&d,event,strlen(event)))return 0;
    int o=xz_json_object_get(&d,0,"values");
    for(int i=0;i<s->nf;i++)if(ext_transport_capacity(s,i)&&
        xz_json_object_get(&d,o,s->fields[i].name)>=0)return 0;
    return 1;
}
static int ext_cache_current(struct ext_schema*s,const char*event) {
    if(!ext_cache_transport_free(s,event))return -1;
    xz_json_doc d;if(xz_json_parse(&d,event,strlen(event)))return -1;
    int o=xz_json_object_get(&d,0,"values");if(o<0)return 0;
    for(int i=0;i<s->nf;i++)if(s->fields[i].dynamic&&xz_json_object_get(&d,o,s->fields[i].name)>=0) {
        if(!ext_text(&d,o,s->fields[i].name,s->current[i],sizeof(s->current[i]),1)||
           !ext_value(s,i,s->current[i]))return -1;
        s->current_mask|=1U<<i;
    }
    return 0;
}
/* Missing/stale cache is 0. Corrupt or untrusted cache is -1, never an empty
 * successful directory. No caller should clear saved configuration on either. */
static int ext_cache_load(struct ext_schema*s,int dir) {
    if(!ext_has_dynamic(s)||dir<0)return 0;
    int fd=openat(dir,"dynamic.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    EXT_OPTIONS_STAT st,owner;int result=-1;char*raw=NULL;
    if(EXT_OPTIONS_FSTAT(dir,&owner)||EXT_OPTIONS_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||
       st.st_uid!=owner.st_uid||st.st_nlink!=1||(st.st_mode&0777)!=0600||
       st.st_size<=0||st.st_size>EXT_OPTIONS_CACHE_CAP)goto done;
    raw=malloc((size_t)st.st_size+1);if(!raw)goto done;
    size_t used=0;ssize_t n;
    while(used<(size_t)st.st_size&&(n=read(fd,raw+used,(size_t)st.st_size-used))>0)used+=(size_t)n;
    if(used!=(size_t)st.st_size||memchr(raw,0,used))goto done;
    raw[used]=0;unsigned a,b,c;int off=0;
    if(sscanf(raw,"%u %u %u%n",&a,&b,&c,&off)!=3||off<=0||raw[off]!='\n'||
       a>EXT_CAP||b>EXT_EVENT_CAP||c>EXT_EVENT_CAP||(size_t)off+1+a+b+c!=used)goto done;
    off++;
    if(a!=strlen(s->json)||memcmp(raw+off,s->json,a)){result=0;goto done;}
    off+=(int)a;
    memcpy(s->options_json,raw+off,b);s->options_json[b]=0;off+=(int)b;
    memcpy(s->current_json,raw+off,c);s->current_json[c]=0;
    if((b&&(!ext_public_schema(s->options_json,s)||!ext_event_has(s->options_json,"options")||
             xz_json_parse(&s->options_doc,s->options_json,b)))||
       (c&&(!ext_public_schema(s->current_json,s)||ext_cache_current(s,s->current_json))))goto done;
    result=b?1:0;
done:
    close(fd);free(raw);
    if(result<0){s->options_json[0]=s->current_json[0]=0;s->current_mask=0;}
    return result;
}
static int ext_cache_write_bytes(int fd,const char*p,size_t n) {
    while(n){ssize_t w=write(fd,p,n);if(w>0){p+=w;n-=(size_t)w;}else if(w<0&&errno==EINTR)continue;else return -1;}
    return 0;
}
static int ext_cache_reset_requested(int dir) {
    int fd=openat(dir,"dynamic.reset",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    EXT_OPTIONS_STAT st,owner;
    int ok=!EXT_OPTIONS_FSTAT(dir,&owner)&&!EXT_OPTIONS_FSTAT(fd,&st)&&S_ISREG(st.st_mode)&&
        st.st_uid==owner.st_uid&&st.st_nlink==1&&(st.st_mode&0777)==0600&&st.st_size==0;
    close(fd);return ok?1:-1;
}
/* The reset marker prevents a running action from republishing its old in-
 * memory snapshot after a platform script explicitly changes account identity.
 * It is cache metadata only; settings, credentials and public state stay put. */
static int ext_cache_forget(int dir) {
    int reset=ext_cache_reset_requested(dir);if(reset<0)return -1;
    if(unlinkat(dir,"dynamic.json",0)&&errno!=ENOENT)return -1;
    if(unlinkat(dir,"dynamic.new",0)&&errno!=ENOENT)return -1;
    if(reset)return 0;
    int fd=openat(dir,"dynamic.reset",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd>=0){close(fd);return 0;}
    return errno==EEXIST&&ext_cache_reset_requested(dir)==1?0:-1;
}
static int ext_cache_append(char*out,size_t*used,const char*p,size_t n) {
    if(*used+n>EXT_EVENT_CAP)return -1;
    memcpy(out+*used,p,n);*used+=n;out[*used]=0;return 0;
}
/* Events may update one dynamic field at a time. Merge validated objects by
 * schema field so UI and server keep the same remaining allowlists. Recheck
 * the resulting complete snapshot against the same global count/byte bounds. */
static int ext_cache_merge(const struct ext_schema*s,const char*key,const char*base,const char*event,char out[EXT_EVENT_CAP+1]) {
    xz_json_doc old,new;int oldobject=-1,newobject;size_t used=0;int count=0;
    if(!ext_public_schema(event,s)||xz_json_parse(&new,event,strlen(event))||
       (newobject=xz_json_object_get(&new,0,key))<0)return -1;
    if(base&&*base) {
        if(!ext_public_schema(base,s)||xz_json_parse(&old,base,strlen(base)))return -1;
        oldobject=xz_json_object_get(&old,0,key);
    }
    if(ext_cache_append(out,&used,"{\"",2)||ext_cache_append(out,&used,key,strlen(key))||ext_cache_append(out,&used,"\":{",3))return -1;
    for(int i=0;i<s->nf;i++) {
        if(!strcmp(key,"options")&&!s->fields[i].dynamic)continue;
        if(!strcmp(key,"values")&&ext_transport_capacity(s,i))continue;
        const xz_json_doc*d=&new;int t=xz_json_object_get(d,newobject,s->fields[i].name);
        if(t<0&&oldobject>=0){d=&old;t=xz_json_object_get(d,oldobject,s->fields[i].name);}
        if(t<0)continue;
        if((count++&&ext_cache_append(out,&used,",",1))||ext_cache_append(out,&used,"\"",1)||
           ext_cache_append(out,&used,s->fields[i].name,strlen(s->fields[i].name))||ext_cache_append(out,&used,"\":",2)||
           (d->tokens[t].type==XZ_JSON_STRING&&ext_cache_append(out,&used,"\"",1))||
           ext_cache_append(out,&used,d->source+d->tokens[t].start,d->tokens[t].end-d->tokens[t].start)||
           (d->tokens[t].type==XZ_JSON_STRING&&ext_cache_append(out,&used,"\"",1)))return -1;
    }
    if(ext_cache_append(out,&used,"}}",2)||!ext_public_schema(out,s))return -1;
    return 0;
}
/* Both pending catalogue and current-value output publish in one rename only
 * after exit 0. A failed, cancelled, malformed or overflowing action retains
 * the previous complete allowlist and current values. */
static int ext_cache_commit(const struct ext_schema*s,int dir,const char*options,const char*current) {
    if(!ext_has_dynamic(s))return 0;
    if(!options||!*options)options=s->options_json;
    if(!current||!*current)current=s->current_json;
    if(!*options&&!*current)return 0;
    if((*options&&!ext_public_schema(options,s))||(*current&&(!ext_public_schema(current,s)||!ext_cache_transport_free(s,current))))return -1;
    char header[64];int n=snprintf(header,sizeof(header),"%zu %zu %zu\n",strlen(s->json),strlen(options),strlen(current));
    if(n<=0||(size_t)n>=sizeof(header))return -1;
    unlinkat(dir,"dynamic.new",0);
    int fd=openat(dir,"dynamic.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    int rc=ext_cache_write_bytes(fd,header,(size_t)n)||ext_cache_write_bytes(fd,s->json,strlen(s->json))||
        ext_cache_write_bytes(fd,options,strlen(options))||ext_cache_write_bytes(fd,current,strlen(current));
    close(fd);
    if(!rc)rc=renameat(dir,"dynamic.new",dir,"dynamic.json");
    if(rc)unlinkat(dir,"dynamic.new",0);
    return rc?-1:0;
}
/* Keeper can recycle an extension slot between actions, changing its UID.
 * While Web still owns the 0700 workspace, rebuild only the snapshot it has
 * already validated, then transfer that new regular file before input publish.
 * Never chown an unvalidated path supplied by the old action. */
static int ext_cache_handoff(const struct ext_schema*s,int dir,uid_t uid) {
    if(unlinkat(dir,"dynamic.reset",0)&&errno!=ENOENT)return -1;
    if(s->options_json[0]||s->current_json[0]) {
        if(ext_cache_commit(s,dir,NULL,NULL))return -1;
    } else if(unlinkat(dir,"dynamic.json",0)&&errno!=ENOENT)return -1;
    int fd=openat(dir,"dynamic.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    EXT_OPTIONS_STAT st;
    int ok=!EXT_OPTIONS_FSTAT(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&
        st.st_nlink==1&&(st.st_mode&0777)==0600&&!fchown(fd,uid,uid);
    close(fd);return ok?0:-1;
}
static int ext_cache_options(const struct ext_schema*s,char out[EXT_EVENT_CAP+1]) {
    strcpy(out,"{}");if(!s->options_json[0])return 0;
    int o=xz_json_object_get(&s->options_doc,0,"options");if(o<0)return -1;
    size_t n=s->options_doc.tokens[o].end-s->options_doc.tokens[o].start;
    if(n>EXT_EVENT_CAP)return -1;
    memcpy(out,s->options_json+s->options_doc.tokens[o].start,n);out[n]=0;return 1;
}
static int ext_cache_values(const struct ext_schema*s,char out[EXT_EVENT_CAP+1]) {
    strcpy(out,"null");if(!s->current_json[0])return 0;
    xz_json_doc d;if(xz_json_parse(&d,s->current_json,strlen(s->current_json)))return -1;
    int o=xz_json_object_get(&d,0,"values");if(o<0)return -1;
    size_t n=d.tokens[o].end-d.tokens[o].start;if(n>EXT_EVENT_CAP)return -1;
    memcpy(out,s->current_json+d.tokens[o].start,n);out[n]=0;return 1;
}
/* This environment variable comes from the managed runner, never a POST body.
 * Direct show/run need no catalogue; a direct save can retain its stored ID or
 * schema seeds, but cannot invent a new selection without a trusted cache. */
static int ext_cache_environment(void) {
    const char*path=getenv("LX06_EXTENSION_DIR");if(!path||!*path){errno=ENOENT;return -1;}
    size_t root=strlen(EXT_OPTIONS_ROOT);
    if(strncmp(path,EXT_OPTIONS_ROOT,root)||strncmp(path+root,"/ext-",5)||
       strlen(path+root+5)!=32||strspn(path+root+5,"0123456789abcdef")!=32){errno=EPERM;return -1;}
    int top=open(EXT_OPTIONS_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(top<0)return -1;
    EXT_OPTIONS_STAT st;
    if(EXT_OPTIONS_FSTAT(top,&st)||st.st_uid!=0||(st.st_mode&0777)!=0755){close(top);errno=EPERM;return -1;}
    int dir=openat(top,path+root+1,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(top);if(dir<0)return -1;
    if(EXT_OPTIONS_FSTAT(dir,&st)||st.st_uid!=getuid()||(st.st_mode&0777)!=0700){close(dir);errno=EPERM;return -1;}
    return dir;
}
#endif
