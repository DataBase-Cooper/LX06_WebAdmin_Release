/* Authenticated Web adapter for declarative script controls. No provider names,
 * login URLs, OAuth secrets, or platform request semantics belong here. */
#ifndef EXT_DIR
#define EXT_DIR "/tmp/lx06-extensions"
#endif
static void ext_job(const char*name,char job[40]) {
    unsigned char hash[16];md5_digest(name,strlen(name),hash);strcpy(job,"ext-");
    for(int i=0;i<16;i++)snprintf(job+4+i*2,3,"%02x",hash[i]);
}
static int ext_running(const char*name) {
    char job[40];ext_job(name,job);
    if(!keeper_status_read(1))return -1;
    for(unsigned i=0;i<keeper_cached.jobs;i++)if(!strcmp(keeper_cached.job[i].name,job))return 1;
    return 0;
}
/* Extension actions use temporary keeper workspaces, not persistent application
 * slots. Release only finished ext-<MD5> jobs; keep /tmp public result state and
 * the script's separate settings/accounts untouched. Also handles closed tabs
 * and Web restarts, where no client remains to poll the completed action. */
static void ext_reap_idle(void) {
    int d=app_trusted_dir("/data/lx06/keeper",0700,0);
    if(d<0)return;
    int fd=app_open_file(d,"slots",O_RDONLY);close(d);
    if(fd<0)return;
    FILE *f=fdopen(fd,"r");if(!f){close(fd);return;}
    if(!keeper_status_read(1)){fclose(f);return;}
    char line[128],name[65];unsigned slot;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"%u %64s",&slot,name)!=2||slot>=LK_MAX_SLOTS||
           strlen(name)!=36||strncmp(name,"ext-",4)||
           strspn(name+4,"0123456789abcdef")!=32)continue;
        int running=0;
        for(unsigned i=0;i<keeper_cached.jobs;i++)
            if(!strcmp(keeper_cached.job[i].name,name))running=1;
        if(running)continue;
        struct lk_request q={0};struct lk_reply r;
        q.magic=LK_MAGIC;q.action=LK_FORGET;strcpy(q.name,name);
        /* Keeper rechecks live jobs/descendants before removing a workspace. */
        (void)lk_call(&q,&r,500);
    }
    fclose(f);keeper_invalidate();
}
static int ext_schema_load(const char*name,struct ext_schema*s) {
    int d=app_trusted_dir(APPPERSIST,0755,0);if(d<0)return 0;
    int f=app_open_file(d,name,O_RDONLY);close(d);if(f<0)return 0;
    int rc=ext_read(f,s);close(f);return rc;
}
static int ext_query_name(const char*q,char name[65]) {
    if(strncmp(q,"name=",5))return 0;
    size_t n=strcspn(q+5,"&");if(n>64||q[5+n])return 0;
    memcpy(name,q+5,n);name[n]=0;return app_name_safe(name);
}
/* The parent directory is root-owned. Child directories may belong only to
 * root or a keeper reserved UID; no symlinks/hardlinks/FIFOs are followed. */
static int ext_dir_open(const char*name,int create,char*path,size_t cap) {
    char job[40];ext_job(name,job);snprintf(path,cap,EXT_DIR "/%s",job);
    int top=app_trusted_dir(EXT_DIR,0755,create);if(top<0)return -1;
    if(create&&mkdirat(top,job,0700)&&errno!=EEXIST){close(top);return -1;}
    int d=openat(top,job,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(top);
    if(d<0)return -1;
    LX06_STATBUF st;
    if(LX06_FSTAT(d,&st)||(st.st_uid!=0&&(st.st_uid<LK_UID_BASE||st.st_uid>=LK_UID_BASE+LK_MAX_SLOTS))||
       (st.st_mode&0777)!=0700){close(d);errno=EPERM;return -1;}
    return d;
}
#ifndef LX06_RECOGNITION_EXTENSION_HTTP_FIXTURE
#include "app-target-settings.h"
#include "native-settings/token-sync-profile.h"
/* One retained original joint read per logged-in modal. A terminal ticket
 * stays spent until an explicit new read; no POST re-reads or retries a save. */
struct ext_native_entry {
    char session[128],ticket[65],name[65],action[33],values[8193];
    int active,spent,scripts,source;time_t created;LX06_STATBUF revision;
    struct rtv_snapshot *loaded;
};
static struct ext_native_entry ext_native_entries[MAXSESS];
struct ext_native_ca {int parent,source,snapshot;char directory[2049],name[256],path[64];LX06_STATBUF dir_stat,stat;};
struct ext_native_call {struct ext_native_entry *entry;int fd;struct rtv_snapshot *saved;struct ext_native_ca *ca;};
static int ext_native_stat(const LX06_STATBUF *a,const LX06_STATBUF *b) {
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&
        a->st_uid==b->st_uid&&a->st_gid==b->st_gid&&a->st_nlink==b->st_nlink&&a->st_size==b->st_size&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static void ext_native_release(struct ext_native_entry *e) {
    if(!e||!e->active)return;
    recognition_target_view_close(e->loaded);e->loaded=NULL;
    if(e->source>=0)close(e->source);if(e->scripts>=0)close(e->scripts);
    e->source=e->scripts=-1;
}
static int ext_native_ca_current(struct ext_native_ca *c) {
    LX06_STATBUF held,named,dir;int parent=app_trusted_dir(c->directory,c->dir_stat.st_mode&0777,0);
    int rc=parent<0||LX06_FSTAT(parent,&dir)||!ext_native_stat(&c->dir_stat,&dir)||
        LX06_FSTAT(c->source,&held)||!ext_native_stat(&c->stat,&held)||
        LX06_STATAT(c->parent,c->name,&named,AT_SYMLINK_NOFOLLOW)||!ext_native_stat(&held,&named);
    if(parent>=0)close(parent);return rc?-1:0;
}
static int ext_native_current(void *ctx) {
    struct ext_native_call *c=ctx;struct ext_native_entry *e=c?c->entry:NULL;
    char cookie[160],holder[128],byte;LX06_STATBUF source,named,dir,held;
    if(!e||!e->active||e->scripts<0||e->source<0||!lx06_scripts_allowed())return -1;
    snprintf(cookie,sizeof cookie,"webd=%s",e->session);
    if(!sess_ok(cookie)||deploy_lock_active(holder,sizeof holder)||ext_running(e->name)!=0)return -1;
    ssize_t peek=recv(c->fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
    if(peek==0||(peek<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))return -1;
    int current=app_trusted_dir(APPPERSIST,0755,0);
    int bad=current<0||LX06_FSTAT(current,&dir)||LX06_FSTAT(e->scripts,&held)||
        dir.st_dev!=held.st_dev||dir.st_ino!=held.st_ino||
        LX06_FSTAT(e->source,&source)||!ext_native_stat(&e->revision,&source)||
        LX06_STATAT(e->scripts,e->name,&named,AT_SYMLINK_NOFOLLOW)||!ext_native_stat(&source,&named);
    if(current>=0)close(current);
    if(bad||(c->saved&&recognition_target_profile_current(c->saved,e->scripts))||
       (c->ca&&ext_native_ca_current(c->ca)))return -1;
    return 0;
}
static int ext_native_random(char out[65]) {
    unsigned char bytes[32];if(rand_bytes(bytes,sizeof bytes))return -1;
    for(unsigned i=0;i<sizeof bytes;i++)snprintf(out+2*i,3,"%02x",bytes[i]);
    memset(bytes,0,sizeof bytes);return 0;
}
static int ext_native_action(const struct ext_schema *s,const char *action) {
    static const char *const keys[]={"host","port","ca_file","spki","token","phone_allowlist","speaker_allowlist"};
    for(int a=0;a<s->na;a++)if(!action||!strcmp(action,s->actions[a].id)) {
        int fields=xz_json_object_get(&s->doc,s->actions[a].token,"fields");unsigned mask=0;
        for(int t=fields<0?-1:s->doc.tokens[fields].first_child;t>=0;t=s->doc.tokens[t].next_sibling)
            for(unsigned k=0;k<7;k++)if(ext_raw_eq(&s->doc,t,keys[k]))mask|=1u<<k;
        if(mask==127)return a;
    }return -1;
}
/* 0 is an ordinary legacy script; 1 is a loaded native entry; -1 refuses a
 * declared native entry whose original dependencies/auth/source are stale. */
static int ext_native_load(int fd,const char *head,const char *name,const struct ext_schema *schema,
        struct ext_native_entry **result) {
    char cookie[512],session[128];hdr_get(head,"Cookie",cookie,sizeof cookie);
    if(!sess_ok(cookie)||sess_token_from_cookie(cookie,session,sizeof session))return -1;
    int scripts=app_trusted_dir(APPPERSIST,0755,0),source=-1;
    if(scripts>=0)source=app_open_file(scripts,name,O_RDONLY);
    if(source<0){if(scripts>=0)close(scripts);return -1;}
    if(!app_target_settings_supported(source)){close(source);close(scripts);return 0;}
    struct ext_schema *held=calloc(1,sizeof *held);
    int same=held&&ext_read(source,held)==1&&!strcmp(held->json,schema->json);free(held);
    if(!same){close(source);close(scripts);return -1;}
    int action=ext_native_action(schema,NULL);
    if(action<0){close(source);close(scripts);return -1;}
    struct ext_native_entry *e=NULL;
    for(unsigned i=0;i<MAXSESS;i++)if(ext_native_entries[i].active&&!strcmp(ext_native_entries[i].session,session)){e=&ext_native_entries[i];break;}
    if(!e)for(unsigned i=0;i<MAXSESS;i++)if(!ext_native_entries[i].active){e=&ext_native_entries[i];break;}
    if(!e){e=&ext_native_entries[0];for(unsigned i=1;i<MAXSESS;i++)if(ext_native_entries[i].created<e->created)e=&ext_native_entries[i];}
    ext_native_release(e);memset(e,0,sizeof *e);e->source=source;e->scripts=scripts;e->active=1;e->created=time(NULL);
    strcpy(e->session,session);strcpy(e->name,name);strcpy(e->action,schema->actions[action].id);
    struct ext_native_call call={e,fd,NULL,NULL};
    if(LX06_FSTAT(source,&e->revision)||ext_native_random(e->ticket)||ext_native_current(&call)||
       app_target_settings_load(scripts,source,name,ext_native_current,&call,&e->loaded,e->values)) {
        ext_native_release(e);memset(e,0,sizeof *e);return -1;
    }
    *result=e;return 1;
}
static void ext_native_ca_close(struct ext_native_ca *c) {
    if(c->snapshot>=0)close(c->snapshot);if(c->source>=0)close(c->source);if(c->parent>=0)close(c->parent);
    c->snapshot=c->source=c->parent=-1;
}
/* Linux 3.10: hold the original trusted CA and an immediately unlinked 0600
 * snapshot. OpenSSL reads the stable FD; every original source gate still runs. */
static int ext_native_ca_open(const char *path,struct ext_native_ca *c) {
    memset(c,0,sizeof *c);c->parent=c->source=c->snapshot=-1;
    if(!path||path[0]!='/'||strlen(path)>=sizeof c->directory)return -1;
    strcpy(c->directory,path);char *slash=strrchr(c->directory,'/');
    if(!slash||!slash[1]||strlen(slash+1)>=sizeof c->name)return -1;
    strcpy(c->name,slash+1);if(slash==c->directory)slash[1]=0;else *slash=0;
    LX06_STATBUF before,after;char bytes[65536];int top=-1;char temporary[100],random[65];int created=0,rc=-1;
    if(LX06_STAT(c->directory,&before)||(c->parent=app_trusted_dir(c->directory,before.st_mode&0777,0))<0||
       LX06_FSTAT(c->parent,&c->dir_stat)||(c->source=app_open_file(c->parent,c->name,O_RDONLY))<0||
       LX06_FSTAT(c->source,&c->stat)||c->stat.st_size<1||c->stat.st_size>(off_t)sizeof bytes)goto done;
    size_t at=0;while(at<(size_t)c->stat.st_size) {
        ssize_t n=pread(c->source,bytes+at,(size_t)c->stat.st_size-at,(off_t)at);
        if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;
    }
    if(LX06_FSTAT(c->source,&after)||!ext_native_stat(&c->stat,&after)||ext_native_ca_current(c)||
       ext_native_random(random)||(top=app_trusted_dir(EXT_DIR,0755,1))<0)goto done;
    snprintf(temporary,sizeof temporary,".ca-%s",random);
    c->snapshot=openat(top,temporary,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(c->snapshot<0)goto done;created=1;
    if(app_regular(c->snapshot)||LX06_FSTAT(c->snapshot,&after)||(after.st_mode&0777)!=0600||
       LX06_STATAT(top,temporary,&before,AT_SYMLINK_NOFOLLOW)||!ext_native_stat(&after,&before)||
       unlinkat(top,temporary,0))goto done;
    created=0;at=0;while(at<(size_t)c->stat.st_size) {
        ssize_t n=write(c->snapshot,bytes+at,(size_t)c->stat.st_size-at);
        if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=(size_t)n;
    }
    if(lseek(c->snapshot,0,SEEK_SET)<0||ext_native_ca_current(c))goto done;
    snprintf(c->path,sizeof c->path,"/proc/self/fd/%d",c->snapshot);rc=0;
done:
    memset(bytes,0,sizeof bytes);if(created)unlinkat(top,temporary,0);if(top>=0)close(top);
    if(rc)ext_native_ca_close(c);return rc;
}
static int ext_native_connection(const struct ext_native_entry *e,const struct ext_schema *s,
        const xz_json_doc *request,int values,char out[8193]) {
    xz_json_doc stored;if(xz_json_parse(&stored,e->values,strlen(e->values)))return -1;
    size_t used=0;int count=0;if(!ext_buf(out,8193,&used,"{",1))return -1;
    for(int k=request->tokens[values].first_child;k>=0;) {
        int v=request->tokens[k].next_sibling;char key[33],value[2049],prior[2049];
        if(v<0||decode_string(request,k,key,sizeof key)||!ext_text(request,values,key,value,sizeof value,0))return -1;
        int f=ext_field_index(s,key);if(f<0)return -1;
        int connection=!strcmp(key,"host")||!strcmp(key,"port")||!strcmp(key,"ca_file")||!strcmp(key,"spki")||
            !strcmp(key,"token")||!strcmp(key,"phone_allowlist")||!strcmp(key,"speaker_allowlist");
        int present=xz_json_object_get(&stored,0,key)>=0;
        if(present&&!ext_text(&stored,0,key,prior,sizeof prior,0))return -1;
        if(!strcmp(key,"token")&&(!*value||!strcmp(value,"@@TOKEN@@"))){k=request->tokens[v].next_sibling;continue;}
        if(!connection) {
            /* This action updates connection only. Ordinary unchanged fields
             * remain under the original joint; a password cannot be compared. */
            if(!strcmp(s->fields[f].type,"password")) {if(*value)return -1;}
            else if(present?strcmp(value,prior):(!ext_text(&s->doc,s->fields[f].token,"default",prior,sizeof prior,0)||strcmp(value,prior)))return -1;
        } else if(!present||strcmp(value,prior)) {
            if((count++&&!ext_buf(out,8193,&used,",",1))||!ext_quote(out,8193,&used,key)||
               !ext_buf(out,8193,&used,":",1)||!ext_quote(out,8193,&used,value))return -1;
        }
        k=request->tokens[v].next_sibling;
    }
    return ext_buf(out,8193,&used,"}",1)?0:-1;
}
static int ext_native_identity(const char *name,char open[512],uint64_t *run,uint64_t *epoch) {
    static uint64_t sequence,space;unsigned char random[16];struct timeval now;
    if(!space) {if(rand_bytes(random,sizeof random))return -1;memcpy(&space,random,sizeof space);space&=UINT64_C(0x3fffffffffffffff);if(!space)space=1;}
    if(sequence==UINT64_C(0x3fffffffffffffff)||rand_bytes(random,sizeof random)||gettimeofday(&now,NULL))return -1;
    ++sequence;*run=space;*epoch=space+sequence;
    uint64_t millis=(uint64_t)now.tv_sec*1000+(unsigned long)now.tv_usec/1000;
    for(unsigned i=0;i<6;i++)random[5-i]=(unsigned char)(millis>>(i*8));
    random[6]=(random[6]&15)|0x70;random[8]=(random[8]&63)|0x80;
    char uuid[37];snprintf(uuid,sizeof uuid,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7],random[8],random[9],random[10],random[11],random[12],random[13],random[14],random[15]);
    int n=snprintf(open,512,"{\"action\":\"caps.request\",\"epoch\":%llu,\"command_id\":\"%s\",\"script_id\":\"%s\",\"wake_source\":\"local\"}",(unsigned long long)*epoch,uuid,name);
    return n>0&&n<512?0:-1;
}
/* Called only after the original declaration/origin/busy/values gates. */
static int ext_native_save(int fd,const char *head,const char *name,const char *action,
        const struct ext_schema *schema,const xz_json_doc *request,int values) {
    if(ext_native_action(schema,action)<0)return 0;
    int d=app_trusted_dir(APPPERSIST,0755,0),f=d<0?-1:app_open_file(d,name,O_RDONLY);
    int supported=f>=0&&app_target_settings_supported(f);if(f>=0)close(f);if(d>=0)close(d);
    if(!supported){char attempted[128];hdr_get(head,"X-LX06-Entry",attempted,sizeof attempted);if(attempted[0]){app_http_error(fd,409,"脚本声明已变化，请重新读取");return 1;}return 0;}
    char cookie[512],session[128],ticket[128];hdr_get(head,"Cookie",cookie,sizeof cookie);hdr_get(head,"X-LX06-Entry",ticket,sizeof ticket);
    if(!sess_ok(cookie)||sess_token_from_cookie(cookie,session,sizeof session)){app_http_error(fd,401,"请先登录");return 1;}
    struct ext_native_entry *e=NULL;
    for(unsigned i=0;i<MAXSESS;i++)if(ext_native_entries[i].active&&!strcmp(ext_native_entries[i].session,session)&&
       !strcmp(ext_native_entries[i].name,name)&&!strcmp(ext_native_entries[i].action,action)&&!strcmp(ext_native_entries[i].ticket,ticket)){e=&ext_native_entries[i];break;}
    if(!e||e->spent||!e->loaded){app_http_error(fd,409,"此提交已使用或来源已变化，请重新读取后再同步");return 1;}
    e->spent=1;char connection[8193];struct ext_native_call call={e,fd,NULL,NULL};
    struct rtv_snapshot *saved=NULL;struct rec_unbind_receipt receipt={0};
    if(ext_native_current(&call)||ext_native_connection(e,schema,request,values,connection)) {
        ext_native_release(e);app_http_error(fd,409,"配置或来源已变化，请重新读取后再同步");return 1;
    }
    int rc=app_target_settings_save_connection(e->scripts,e->source,name,e->loaded,connection,strlen(connection),ext_native_current,&call,&saved,&receipt);
    memset(connection,0,sizeof connection);
    if(rc) {
        recognition_target_view_close(saved);ext_native_release(e);
        send_json(fd,rc==2?"{\"ok\":false,\"native_token\":true,\"saved\":true,\"message\":\"设置已保存，解绑尚未完成；请重新读取，未进行试拨\"}":
            rc==1?"{\"ok\":false,\"native_token\":true,\"saved\":null,\"message\":\"保存结果未确认；请重新读取，未进行试拨\"}":
            "{\"ok\":false,\"native_token\":true,\"saved\":false,\"message\":\"保存未完成；请重新读取，未进行试拨\"}");return 1;
    }
    struct ts_profile profile;memset(&profile,0,sizeof profile);struct ext_native_ca ca;
    ca.parent=ca.source=ca.snapshot=-1;struct ts_b16 *bridge=NULL;int outcome=TS_SYNC_UNCONFIRMED;uint64_t run,epoch;char open[512];int borrowed=0;
    call.saved=saved;
    if(saved&&!recognition_target_profile_borrow(saved,e->scripts,&profile)) {
        borrowed=1;
        if(!ext_native_current(&call)&&!ext_native_ca_open(profile.ca_file,&ca)) {
            call.ca=&ca;bridge=ts_b16_new();
            if(bridge&&!ext_native_identity(name,open,&run,&epoch))outcome=ts_b16_caps_saved(bridge,&profile,ca.path,run,epoch,open,strlen(open),ext_native_current,&call);
        }
    }
    ts_b16_free(bridge);ext_native_ca_close(&ca);if(borrowed)recognition_target_profile_release(saved);
    memset(&profile,0,sizeof profile);recognition_target_view_close(saved);ext_native_release(e);
    if(outcome==TS_SYNC_READY)send_json(fd,"{\"ok\":true,\"native_token\":true,\"saved\":true,\"ready\":true,\"message\":\"设置已保存，音箱端连接与能力确认成功\"}");
    else if(outcome==TS_SYNC_TOKEN_INVALID)send_json(fd,"{\"ok\":false,\"native_token\":true,\"saved\":true,\"code\":4001,\"message\":\"设置已保存，token 已失效（4001）；请获取新 token 后重新读取并同步\"}");
    else send_json(fd,"{\"ok\":false,\"native_token\":true,\"saved\":true,\"message\":\"设置已保存，连接与能力尚未确认；请检查证书与手机服务后重新读取\"}");
    return 1;
}
#endif

static void h_extension_schema(int fd,const char*query
#ifndef LX06_RECOGNITION_EXTENSION_HTTP_FIXTURE
    ,const char*head
#endif
) {
    char name[65];struct ext_schema*s=calloc(1,sizeof(*s));
    if(!s){app_http_error(fd,503,"内存不足");return;}
    if(!ext_query_name(query,name)||ext_schema_load(name,s)!=1) {
        free(s);app_http_error(fd,400,"脚本没有有效的扩展声明");return;
    }
    char response[3*EXT_CAP+512];
#ifndef LX06_RECOGNITION_EXTENSION_HTTP_FIXTURE
    struct ext_native_entry *entry=NULL;int native=ext_native_load(fd,head,name,s,&entry);
    if(native<0){free(s);app_http_error(fd,409,"配置尚不能安全读取，请确认脚本与设置依赖后重新读取");return;}
    if(native)snprintf(response,sizeof response,"{\"ok\":true,\"copy_codes\":%d,\"schema\":%s,\"native_token\":{\"ticket\":\"%s\",\"action\":\"%s\",\"values\":%s}}",!!(app_file_marker_flags(name)&APP_COPY_CODES),s->json,entry->ticket,entry->action,entry->values);
    else
#endif
    snprintf(response,sizeof(response),"{\"ok\":true,\"copy_codes\":%d,\"schema\":%s}",!!(app_file_marker_flags(name)&APP_COPY_CODES),s->json);
    free(s);send_json(fd,response);
}
static void h_extension_state(int fd,const char*query) {
    char name[65],path[128],buf[EXT_EVENT_CAP+256],response[3*EXT_EVENT_CAP+512],options[EXT_EVENT_CAP+1]="{}",values[EXT_EVENT_CAP+1]="null";
    if(!ext_query_name(query,name)){app_http_error(fd,400,"名称不合法");return;}
    int running=ext_running(name);
    if(running<0){app_http_error(fd,503,"无法读取扩展任务状态");return;}
    int d=ext_dir_open(name,0,path,sizeof(path)),had_dir=d>=0,f=-1;ssize_t n=-1;LX06_STATBUF st;
    struct ext_schema *schema=calloc(1,sizeof(*schema));
    if(schema&&ext_schema_load(name,schema)!=1){free(schema);schema=NULL;}
    if(d>=0) {
        if(schema){(void)ext_cache_load(schema,d);(void)ext_cache_options(schema,options);(void)ext_cache_values(schema,values);}
        f=openat(d,"state.json",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
        if(f>=0 && !LX06_FSTAT(f,&st)&&S_ISREG(st.st_mode)&&st.st_nlink==1&&
           !(st.st_mode&0077))n=read(f,buf,sizeof(buf)-1);
        if(f>=0)close(f);
        close(d);
    }
    int valid=0;
    if(n>0&&(size_t)n<sizeof(buf)-1) {
        buf[n]=0;xz_json_doc doc;
        if(!xz_json_parse(&doc,buf,(size_t)n)&&ext_keys(&doc,0,"|running||exit||values_updated||event|")) {
            int ev=xz_json_object_get(&doc,0,"event"),ec;
            int fresh=xz_json_object_get(&doc,0,"values_updated");
            if(ev>=0&&doc.tokens[ev].type==XZ_JSON_OBJECT&&
               (fresh<0||(doc.tokens[fresh].type==XZ_JSON_PRIMITIVE&&(ext_raw_eq(&doc,fresh,"true")||ext_raw_eq(&doc,fresh,"false"))))&&
               !xz_json_get_int(&doc,0,"exit",&ec)) {
                size_t z=doc.tokens[ev].end-doc.tokens[ev].start;char event[EXT_EVENT_CAP+1];
                if(z<sizeof(event)) {
                    memcpy(event,buf+doc.tokens[ev].start,z);event[z]=0;
                    int public_ok=ext_public_schema(event,schema);
                    if(public_ok) {
                        int finished=ext_raw_eq(&doc,xz_json_object_get(&doc,0,"running"),"false");
                        snprintf(response,sizeof(response),"{\"ok\":true,\"copy_codes\":%d,\"running\":%s,\"exit\":%d,\"options\":%s,\"values\":%s,\"event\":%s}",
                                 !!(app_file_marker_flags(name)&APP_COPY_CODES),running?"true":"false",(!running&&!finished)?125:ec,options,
                                 !running&&finished&&!ec&&ext_raw_eq(&doc,xz_json_object_get(&doc,0,"values_updated"),"true")?values:"null",event);valid=1;
                    }
                }
            }
        }
    }
    if(!valid)snprintf(response,sizeof(response),"{\"ok\":true,\"running\":%s,\"exit\":%d,\"options\":%s,\"values\":%s,\"event\":{\"message\":\"%s\"}}",
        running?"true":"false",running?0:(had_dir?125:0),options,"null",running?"正在启动…":(had_dir?"上次操作没有完成，请重试":"请选择操作"));
    free(schema);
    send_json(fd,response);
}
static int ext_same_origin(const char*head) {
    char ct[128],origin[512],host[256],site[64],expected[300];
    hdr_get(head,"Content-Type",ct,sizeof(ct));hdr_get(head,"Origin",origin,sizeof(origin));
    hdr_get(head,"Host",host,sizeof(host));hdr_get(head,"Sec-Fetch-Site",site,sizeof(site));
    if(strncasecmp(ct,"application/json",16)||(ct[16]&&ct[16]!=';')||!strcmp(site,"cross-site"))return 0;
    if(!origin[0])return 1;
    snprintf(expected,sizeof(expected),"http://%s",host);if(!strcmp(origin,expected))return 1;
    snprintf(expected,sizeof(expected),"https://%s",host);return !strcmp(origin,expected);
}
static void h_extension_action(int fd,const char*head,const char*body) {
    if(!lx06_scripts_allowed()){app_http_error(fd,403,"用户脚本开关已关闭，禁止调用扩展脚本");return;}
    char name[65],action[33],job[40],path[128],runner[512],script[256],holder[128];
    struct ext_schema*s=NULL;struct lk_request q;struct lk_reply r;
    xz_json_doc request;
    int d=-1,f=-1,started=0;uid_t uid=0;const char*error="扩展操作启动失败";
    if(!ext_same_origin(head)){app_http_error(fd,403,"扩展操作需要同源 JSON 请求");return;}
    if(strlen(body)>EXT_CAP||xz_json_parse(&request,body,strlen(body))||
       !ext_keys(&request,0,"|name||action||values|")||
       !ext_text(&request,0,"name",name,sizeof(name),1)||!app_name_safe(name)) {
        app_http_error(fd,400,"扩展请求或名称不合法");return;
    }
    if(deploy_lock_active(holder,sizeof(holder))||ext_running(name)!=0) {app_http_error(fd,409,"扩展或部署正在进行，请稍后重试");return;}
    s=calloc(1,sizeof(*s));
    if(s&&ext_schema_load(name,s)==1) {
        int cachedir=ext_dir_open(name,0,path,sizeof(path));
        if(cachedir>=0){(void)ext_cache_load(s,cachedir);close(cachedir);}
        char saved[SETTINGS_CAP+1];int settings=settings_dir(0);
        if((settings<0&&errno!=ENOENT)||ext_settings_load(s,settings,name,saved)<0) {if(settings>=0)close(settings);free(s);s=NULL;}
        else if(settings>=0)close(settings);
    } else {free(s);s=NULL;}
    if(!s||!ext_request(s,body,action)) {free(s);app_http_error(fd,400,"扩展参数与脚本声明或当前模型目录不符");return;}
    /* The legacy account mode is unverified and read-only in the UI.  Enforce
     * the same boundary before publishing input or starting any script. */
    int values=xz_json_object_get(&request,0,"values");
    for(int i=0;i<s->nf;i++) {
        char env[65];
        if(!ext_text(&s->doc,s->fields[i].token,"env",env,sizeof(env),0)) {
            free(s);app_http_error(fd,400,"扩展声明无效");return;
        }
        if((!strcmp(s->fields[i].name,"MODEL_BARGE_ANY")||!strcmp(env,"MODEL_BARGE_ANY"))&&
           xz_json_object_get(&request,values,s->fields[i].name)>=0) {
            free(s);app_http_error(fd,403,"打断方式尚未验证，不能通过扩展操作修改");return;
        }
    }
    /* Candidate borrows the complete owner only AFTER the original route,
     * origin, script declaration and busy checks. It never starts a keeper
     * job. No production registration or second settings writer. */
#ifdef LX06_INTERACTIVE_FLOW_FIXTURE
    if(!strncmp(action,"flow_",5)) {
        /* Inputs travel inside the existing declared edit field, after ALL
         * original gates above. The original saved-object owner must prepare
         * them before start/new; describe/readback never restore input values.
         * Browser data is never runtime proof or source SHA. */
        if(strcmp(action,"flow_describe")&&strcmp(action,"flow_run")&&strcmp(action,"flow_new")&&
           strcmp(action,"flow_poll")&&strcmp(action,"flow_result")&&strcmp(action,"flow_retire")&&
           strcmp(action,"flow_readback")) {free(s);app_http_error(fd,400,"Unknown flow action");return;}
        uf_validated_action(fd,name,action,body);free(s);return;
    }
#else
    if(!strncmp(action,"flow_",5)) {
        free(s);app_http_error(fd,403,"候选流程接口未启用");return;
    }
#endif
#ifdef LX06_TOKEN_ENTRY_FIXTURE
    ea_validated_save(fd,name,action,body);free(s);return;
#else
    if(!strncmp(action,"entry_",6)) {
        free(s);app_http_error(fd,403,"候选设置接口未启用");return;
    }
#endif
#ifndef LX06_RECOGNITION_EXTENSION_HTTP_FIXTURE
    if(ext_native_save(fd,head,name,action,s,&request,values)){free(s);return;}
#endif
    ext_reap_idle();ext_job(name,job);
    ssize_t len=readlink("/proc/self/exe",runner,sizeof(runner)-1);if(len<=0||(size_t)len>=sizeof(runner)-1)goto fail;
    runner[len]=0;snprintf(script,sizeof(script),APPPERSIST "/%s",name);
    d=ext_dir_open(name,1,path,sizeof(path));if(d<0)goto fail;
    if(fchown(d,0,0)||fchmod(d,0700))goto fail;
    unlinkat(d,"input.json",0);unlinkat(d,"state.json",0);unlinkat(d,"state.new",0);
    /* Prepare input under a root-only name; publish after keeper assigns UID. */
    unlinkat(d,"input.new",0);
    if(app_atomic(d,"input.new",body,strlen(body),0600))goto fail;
    if(ext_save_state(d,1,0,"{\"message\":\"正在启动…\"}"))goto fail;
    memset(&q,0,sizeof(q));q.magic=LK_MAGIC;q.action=LK_START;q.flags=LK_LOG_OFF;
    if((app_file_marker_flags(name)&APP_RUN_ROOT)||
       (!strcmp(action,"regenerate-greeting")&&ext_greeting_field(s)>=0))q.flags|=LK_ROOT;
    strcpy(q.name,job);q.argc=5;
    strcpy(q.argv[0],runner);strcpy(q.argv[1],"--extension-run");strcpy(q.argv[2],path);
    strcpy(q.argv[3],script);strcpy(q.argv[4],action);
    if(lk_call(&q,&r,500))goto fail;
    started=1;
    if(!keeper_status_read(1))goto fail;
    int found=0;
    for(unsigned i=0;i<keeper_cached.jobs;i++)if(!strcmp(keeper_cached.job[i].name,job)) {uid=keeper_cached.job[i].uid;found=1;}
    if(!found)goto fail;
    if(ext_cache_handoff(s,d,uid))goto fail;
    f=app_open_file(d,"input.new",O_RDONLY);if(f<0||fchown(f,uid,uid))goto fail;
    close(f);f=-1;
    if(fchown(d,uid,uid)||renameat(d,"input.new",d,"input.json"))goto fail;
    close(d);free(s);keeper_invalidate();send_json(fd,"{\"ok\":true,\"running\":true}");return;
fail:
    free(s);
    if(f>=0)close(f);
    if(d>=0){unlinkat(d,"input.new",0);unlinkat(d,"input.json",0);close(d);}
    if(started){memset(&q,0,sizeof(q));q.magic=LK_MAGIC;q.action=LK_STOP;strcpy(q.name,job);lk_call(&q,&r,500);}
    keeper_invalidate();app_http_error(fd,503,error);
}
/* Uninstall stops/forgets only the associated extension job. EBUSY asks the
 * caller to retry instead of removing files while an action still runs. */
static int ext_forget(const char*name) {
    struct lk_request q;struct lk_reply r;char job[40],path[128];ext_job(name,job);
    memset(&q,0,sizeof(q));q.magic=LK_MAGIC;q.action=LK_FORGET;strcpy(q.name,job);
    if(lk_call(&q,&r,500)&&errno!=ENOENT)return -1;
    int d=ext_dir_open(name,0,path,sizeof(path));
    if(d>=0){unlinkat(d,"input.new",0);unlinkat(d,"input.json",0);unlinkat(d,"state.new",0);unlinkat(d,"state.json",0);unlinkat(d,"dynamic.new",0);unlinkat(d,"dynamic.json",0);unlinkat(d,"dynamic.reset",0);close(d);rmdir(path);}
    keeper_invalidate();return 0;
}
