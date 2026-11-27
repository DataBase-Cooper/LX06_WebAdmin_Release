#define _GNU_SOURCE
#include "phone.h"
#include "../protocol.h"
#include <stdlib.h>
#include <signal.h>
#include <sys/resource.h>
#include "../mode_gate.h"
#if defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) || defined(LX06_PHONE_PROFILE_RUNTIME)
#include "../bridge/profile_owner.h"
#endif
/* Called with the receiver's source and its retained native matching selection.
 * The owner consumes that selection before replacing any session. The old inherited
 * fd/PHONE_PAIRING_CONFIG probe remains separate, with no promotion to B16. */
int dc_phone_selected_start(struct br_profile_owner *owner,struct eo_saved_source *source,
        const dc_mode_request *request,uint64_t run,uint64_t epoch,const void *open,size_t n) {
#if defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) || defined(LX06_PHONE_PROFILE_RUNTIME)
    return br_profile_owner_start(owner,source,request,run,epoch,open,n);
#else
    (void)owner;(void)source;(void)request;(void)run;(void)epoch;(void)open;(void)n;
    return -1; /* T10/T14 production registration remains refused. */
#endif
}
#ifndef DC_PHONE_WORKER_EMBEDDED
int main(int argc,char **argv) {
    (void)argv;
    struct rlimit zero={0,0};
    if(argc!=1||setrlimit(RLIMIT_CORE,&zero)||signal(SIGPIPE,SIG_IGN)==SIG_ERR)return 2;
    return dc_phone_run(DC_ADAPTER_FD,getenv("PHONE_PAIRING_CONFIG"));
}

#endif /* DC_PHONE_WORKER_EMBEDDED */

#ifdef LX06_PHONE_PROFILE_RUNTIME
#include "app-extension-objects.h"
#include "selected-script-profile.h"
#include "../bridge/result_inbox.h"
#include "../io/io.h"
#include "../control.h"
#include "../../common/unicode-nfc.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/timex.h>
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct timex)==128 && __builtin_offsetof(struct timex,status)==20, "old ARM timex layout");
_Static_assert(sizeof(((struct timex *)0)->time.tv_sec)==4, "old ARM time32 clock");
#endif
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

struct dc_phone_runtime {
    struct eo_owner *objects;struct eo_saved_source source;br_profile_owner *owner;
    br_store store;br_inbox *inbox;br_result identity;
    dc_mode_request request;uint64_t run,epoch,started;
    char caps[1024],open[1024];size_t caps_bytes,open_bytes;
    int sent,terminal,durable,close_requested,done,failed,store_open;
    dc_phone_media_io media;unsigned requested_audio;uint64_t audio_sequence;
    uint64_t microphone_birth;uint32_t microphone_pid;int microphone_seen;
};
static int phone_media(void *ctx,uint64_t run,uint64_t epoch,const void *raw,size_t n){
    struct dc_phone_runtime *p=ctx;dc_wire_packet packet;
    const br_operation *op=p?br_profile_owner_operation(p->owner):NULL;
    if(!p||run!=p->run||epoch!=p->epoch||!p->terminal||!p->durable||p->close_requested||
       !op||!op->completed||op->plan.session_end||!(p->requested_audio&CFG_AUDIO_SPEAKER)||
       op->confirmed_audio!=p->requested_audio||!p->media.audio)return -1;
    /* The original Wire decoder enforces epoch/PCM shape. Remote generation0
     * belongs to this one operation; it is never used as a local player tag. */
    if(br_wire_decode(raw,n,BR_FROM_PHONE,epoch,NULL,NULL,&packet)!=BR_WIRE_OK||
       packet.header.type!=BR_WIRE_AUDIO||packet.header.generation||
       packet.header.flags!=DC_WIRE_PCM_S16LE||packet.header.channels!=1||
       (packet.header.rate!=8000&&packet.header.rate!=16000&&packet.header.rate!=24000&&packet.header.rate!=48000))return -1;
    if(packet.header.sequence<=p->audio_sequence)return 0;
    if(p->media.audio(p->media.context,&packet)!=1)return -1;
    p->audio_sequence=packet.header.sequence;return 1;
};
static int phone_digest(const void *data,size_t length,char out[65]){
    unsigned char value[32];if(!SHA256(data,length,value))return 0;
    for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",value[i]);return 1;
}
/* Reuse the original Unicode 15.1 validator for every retained carrier. */
static int phone_identity_same(const struct dc_phone_runtime *p,const br_result *r){
    return p&&r&&r->origin==1&&r->epoch==p->identity.epoch&&r->local.serial==p->identity.local.serial&&
        !strcmp(r->command_id,p->identity.command_id)&&!strcmp(r->local.script,p->identity.local.script)&&
        !strcmp(r->local.revision,p->identity.local.revision)&&!strcmp(r->local.task,p->identity.local.task);
}
static int phone_associate(void *ctx,const br_result *r){return phone_identity_same(ctx,r);}
static int phone_remote_associate(void *ctx,const br_result *r){(void)ctx;(void)r;return 0;}
static int phone_receipt(void *ctx,uint64_t run,uint64_t epoch,const char *command,const char *task){
    struct dc_phone_runtime *p=ctx;const br_operation *op=br_profile_owner_operation(p->owner);
    if(!p||run!=p->run||epoch!=p->epoch||!op||!p->durable||strcmp(command,p->identity.command_id)||
       strcmp(task,op->task_id))return 0;
    return br_inbox_sync_receipt(p->inbox,&p->identity,0);
}
static int phone_reaped(void *ctx,uint64_t run,uint64_t epoch,const char *command,const char *task){
    struct dc_phone_runtime *p=ctx;
    /* The profile belongs to this command. Media is borrowed from the original
     * main player, which must observe exact STOPPED and its emptied PCM slot.
     * Neither job CLOSED nor a PID/name establishes these per-command facts. */
    return p&&p->sent&&p->terminal&&p->close_requested&&
        p->media.reaped&&p->media.reaped(p->media.context,run,epoch)==1&&
        br_profile_owner_transport_closed(p->owner,run,epoch)==1&&
        phone_receipt(ctx,run,epoch,command,task)==1;
}
static void phone_close_requested(void *ctx,uint64_t run,uint64_t epoch){
    struct dc_phone_runtime *p=ctx;if(p&&p->run==run&&p->epoch==epoch)p->close_requested=1;
}
static uint64_t phone_wall(int *trusted){
    struct timespec t;struct timex state={0};*trusted=0;
    if(clock_gettime(CLOCK_REALTIME,&t))return 0;
    int64_t seconds=(int64_t)t.tv_sec;
    if(seconds<INT64_C(1704067200)||seconds>=INT64_C(4102444800))return 0;
    int rc=adjtimex(&state);*trusted=rc>=0&&rc!=TIME_ERROR&&!(state.status&STA_UNSYNC);
    return (uint64_t)seconds*1000u+(unsigned long)t.tv_nsec/1000000u;
}
static int phone_uuid(char out[37],uint64_t wall){
    unsigned char u[16];if(RAND_bytes(u,sizeof u)!=1)return -1;
    for(unsigned i=0;i<6;i++)u[5-i]=(unsigned char)(wall>>(8*i));u[6]=(u[6]&15)|0x70;u[8]=(u[8]&63)|0x80;
    return snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15])==36?0:-1;
}
static int phone_open_directory(int parent,const char *name){
    return openat(parent,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
}
/* The result store is a runtime-owned output, unlike uploaded script/flow
 * inputs. Bootstrap only its own private directory; never repair or replace
 * an existing object. The original br_store still validates and locks it. */
static int phone_results_directory(int parent){
    struct stat above,opened,named;int created=0;
    if(fstat(parent,&above)||!S_ISDIR(above.st_mode)||above.st_uid!=geteuid()||
       (above.st_mode&0022))return -1;
    if(mkdirat(parent,"bridge-results",0700)){if(errno!=EEXIST)return -1;}
    else created=1;
    int fd=phone_open_directory(parent,"bridge-results");
    if(fd<0)return -1;
    if(fstat(fd,&opened)||!S_ISDIR(opened.st_mode)||opened.st_uid!=geteuid()||
       (opened.st_mode&07777)!=0700||
       fstatat(parent,"bridge-results",&named,AT_SYMLINK_NOFOLLOW)||
       opened.st_dev!=named.st_dev||opened.st_ino!=named.st_ino||
       opened.st_mode!=named.st_mode||opened.st_uid!=named.st_uid||
       (created&&fsync(parent))){close(fd);return -1;}
    return fd;
}
struct dc_phone_runtime *dc_phone_runtime_open(const dc_mode_request *request,uint64_t run,uint64_t epoch,const dc_phone_media_io *media){
    if(!request||!run||!epoch||!media||!media->audio||!media->reaped||strcmp(request->action,"call.start"))return NULL;
    struct dc_phone_runtime *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->run=run;p->epoch=epoch;p->request=*request;p->media=*media;p->store.directory=p->store.lock=-1;
    p->objects=eo_new();p->owner=br_profile_owner_new((br_session_io){p,phone_receipt,phone_reaped,phone_close_requested});
    p->inbox=calloc(1,sizeof(*p->inbox));
    const char *root=getenv("LX06_PHONE_DATA_ROOT");if(!root||!*root)root="/data/lx06";
    int parent=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW),d[2]={-1,-1};
    const char *entries[]={"apps","script-settings"};
    char record[80];snprintf(record,sizeof record,"%s.json",request->script);
    const char *names[]={request->script,record};int parents[]={parent,parent};
    for(unsigned i=0;i<2&&parent>=0;i++)d[i]=phone_open_directory(parent,entries[i]);
    int valid=p->objects&&p->owner&&p->inbox&&parent>=0;
    for(unsigned i=0;i<2;i++)if(d[i]<0)valid=0;
    /* A plain call owns exactly its real script and settings. It neither
     * reads nor fabricates a FLOW carrier; typed FLOW sources stay separate. */
    if(valid)valid=eo_saved_entry_load(&p->source,p->objects,d,parents,entries,names,1,phone_digest)==0;
    for(unsigned i=0;i<2;i++)if(d[i]>=0)close(d[i]);
    if(valid)valid=selected_script_match(&p->source,request->source,request->wake,request->target,run,epoch)==1;
    int trusted=0;uint64_t wall=phone_wall(&trusted);
    if(valid)valid=trusted&&phone_uuid(p->identity.command_id,wall)==0;
    if(valid){
        p->identity.origin=1;p->identity.epoch=epoch;p->identity.local.serial=run;
        strcpy(p->identity.local.script,request->script);strcpy(p->identity.local.revision,p->objects->origin.settings);
        unsigned n=0;for(unsigned i=0;i<36;i++)if(p->identity.command_id[i]!='-')p->identity.local.task[n++]=p->identity.command_id[i];
        int fd=phone_results_directory(parent);
        if(fd<0||br_store_open(&p->store,fd))valid=0;else p->store_open=1;
        if(fd>=0)close(fd);
        if(valid)valid=br_inbox_open_sync(p->inbox,&p->store,phone_remote_associate,phone_associate,p)==0;
    }
    if(parent>=0)close(parent);
    const char *source=!strcmp(request->source,"local_wake")?"local":!strcmp(request->source,"official_asr")?"official":NULL;
    if(valid&&source){
        char wake[193]={0};if(dc_mode_escape_text(request->wake,wake,sizeof wake))valid=0;
        int n=snprintf(p->caps,sizeof p->caps,"{\"action\":\"caps.request\",\"target\":\"system\",\"params\":{},\"epoch\":%llu,\"command_id\":\"%s\",\"script_id\":\"%s\",\"wake_source\":\"%s\",\"wake_text\":\"%s\"}",
            (unsigned long long)epoch,p->identity.command_id,request->script,source,wake);
        valid=n>0&&(size_t)n<sizeof p->caps;p->caps_bytes=valid?(size_t)n:0;
        n=snprintf(p->open,sizeof p->open,"{\"action\":\"%s.%s\",\"target\":\"%s\",\"params\":{},\"epoch\":%llu,\"command_id\":\"%s\",\"script_id\":\"%s\",\"wake_source\":\"%s\",\"wake_text\":\"%s\"}",
            request->target,request->action,request->target,(unsigned long long)epoch,p->identity.command_id,request->script,source,wake);
        valid=valid&&n>0&&(size_t)n<sizeof p->open;p->open_bytes=valid?(size_t)n:0;
        if(valid)valid=br_profile_owner_start_one(p->owner,&p->source,request,run,epoch,p->caps,p->caps_bytes)==BR_PROFILE_CONNECTED;
    }else valid=0;
    if(!valid){dc_phone_runtime_close(p);(void)dc_phone_runtime_destroy(p);return NULL;}return p;
}
/* Declaration/configuration ownership remains with the same retained source.
 * The original word matcher accepts comma lists; this only adapts the formal
 * BARGEIN separator, leaving word bytes and all other dc_config fields alone. */
int dc_phone_runtime_configure(struct dc_phone_runtime *p,struct dc_config *config){
    struct cfg_bridge_app app;
    if(!p||!config||!dc_config_valid(config)||
       br_profile_owner_controls(p->owner,p->run,p->epoch,&app)||!app.control.bargein)return -1;
    if(!app.control.enabled||!app.control.bargein[0])return 0;
    size_t n=strnlen(app.control.bargein,sizeof config->interrupt);
    if(n>=sizeof config->interrupt)return -1;
    dc_config next=*config;
    for(size_t i=0;i<n;i++)next.interrupt[i]=app.control.bargein[i]=='|'?',':app.control.bargein[i];
    next.interrupt[n]=0;
    if(!dc_config_valid(&next))return -1;
    *config=next;return 0;
}
static int phone_store_terminal(struct dc_phone_runtime *p,const br_operation *op,int event){
    br_result result=p->identity;br_text text=br_json_text(&op->response,br_json_field(&op->response,1,event==BR_OP_ERROR?"msg":"text"));
    if(text.length>=sizeof result.text)return -1;if(text.length)memcpy(result.text,text.data,text.length);
    if(event==BR_OP_ERROR){if(!op->error||strlen(op->error)>=sizeof result.code)return -1;strcpy(result.code,op->error);}
    int trusted=0;uint64_t wall=phone_wall(&trusted);
    int rc=br_inbox_sync_receive(p->inbox,&result,wall,trusted,0,86400);
    memset(&result,0,sizeof result);return rc==BR_ACCEPTED||rc==BR_ACCEPTED_SILENT||rc==BR_DUPLICATE?1:-1;
}
int dc_phone_runtime_tick(struct dc_phone_runtime *p,uint64_t now){
    if(!p||p->done)return p&&!p->failed?1:-1;
    if(p->close_requested){
        dc_phone_runtime_close(p);
        return p->done?(p->failed?-1:1):p->terminal&&p->durable?2:-1;
    }
    if(!p->started)p->started=now;
    /* This bounds setup only. A genuine kept READY is governed by the
     * original heartbeat and the main's overall session lifetime. */
    if(now<p->started||(!p->terminal&&now-p->started>45000)){p->failed=1;dc_phone_runtime_close(p);return -1;}
    int event=br_profile_owner_pump_media(p->owner,p->run,p->epoch,phone_media,p);
    const br_operation *op=br_profile_owner_operation(p->owner);
    if(!p->sent&&event==BR_DISC_CAPS){
        struct cfg_bridge_app controls;
        /* Absent audio declarations are unspecified. This normal microphone/
         * speaker path requires the complete explicit vector and ring closed;
         * never downgrade an open/unspecified microphone to another mode. */
        if(br_profile_owner_controls(p->owner,p->run,p->epoch,&controls)||controls.audio_present!=7u||
           (controls.audio_open&~(CFG_AUDIO_MIC|CFG_AUDIO_SPEAKER))){
            p->failed=1;dc_phone_runtime_close(p);return -1;
        }
        p->requested_audio=controls.audio_open;
        /* Carry the original explicit declaration over the frozen OPEN audio
         * vector. READY confirmation remains independent, never inferred from
         * this request. Caps request itself has no audio override. */
        if(!p->open_bytes||p->open[p->open_bytes-1]!='}'){
            p->failed=1;dc_phone_runtime_close(p);return -1;
        }
        int added=snprintf(p->open+p->open_bytes-1,sizeof p->open-p->open_bytes+1,
            ",\"audio\":{\"mic\":\"%s\",\"speaker\":\"%s\",\"ring\":\"close\"}}",
            p->requested_audio&CFG_AUDIO_MIC?"open":"close",
            p->requested_audio&CFG_AUDIO_SPEAKER?"open":"close");
        if(added<=0||(size_t)added>=sizeof p->open-p->open_bytes+1){
            p->failed=1;dc_phone_runtime_close(p);return -1;
        }
        p->open_bytes=p->open_bytes-1+(size_t)added;
        if(!op||br_profile_owner_send_one(p->owner,p->run,p->epoch,p->open,p->open_bytes)!=1){p->failed=1;dc_phone_runtime_close(p);return -1;}
        p->sent=1;return 0;
    }
    if(p->sent&&(event==BR_OP_READY||event==BR_OP_ERROR)){
        p->terminal=1;p->durable=phone_store_terminal(p,op,event)==1;
        p->failed=event==BR_OP_ERROR||!p->durable||op->confirmed_audio!=p->requested_audio;
        /* READY is the command terminal, not a kept media-session close.
         * Keep the original authenticated socket/source until actual media or
         * the existing caller requests close. No second business OPEN here. */
        if(!p->failed&&event==BR_OP_READY&&!op->plan.session_end)return 0;
        dc_phone_runtime_close(p);
        return p->done?(p->failed?-1:1):p->failed?-1:2;
    }
    if(event<0){p->failed=1;dc_phone_runtime_close(p);return -1;}return 0;
}
int dc_phone_runtime_microphone_allowed(struct dc_phone_runtime *p){
    if(!p||!p->owner||!p->sent||p->failed||p->close_requested||p->done||
       (p->terminal&&!p->durable))return 0;
    return br_profile_owner_microphone_allowed(p->owner,p->run,p->epoch);
}
void dc_phone_runtime_microphone_revoke(struct dc_phone_runtime *p){
    if(p&&p->owner)br_profile_owner_microphone_revoke(p->owner,p->run,p->epoch);
}
int dc_phone_runtime_microphone(struct dc_phone_runtime *p,const struct dc_io_capture_frame *frame){
    if(!frame||dc_phone_runtime_microphone_allowed(p)!=1)return -1;
    if(frame->rate!=16000||(frame->bytes!=320&&frame->bytes!=640&&frame->bytes!=960)||
       !frame->sequence||(frame->mode!=DC_IO_CAPTURE_LXA1&&frame->mode!=DC_IO_CAPTURE_LXA2)||
       (frame->flags&DC_IO_CAPTURE_METADATA_INVALID))return -1;
    if(frame->mode==DC_IO_CAPTURE_LXA2){
        if(!frame->producer_pid||!frame->producer_birth_ticks||!frame->available_mono_ms)return -1;
        if(p->microphone_seen&&(p->microphone_pid!=frame->producer_pid||p->microphone_birth!=frame->producer_birth_ticks))return -1;
        p->microphone_seen=1;p->microphone_pid=frame->producer_pid;p->microphone_birth=frame->producer_birth_ticks;
    }
    return br_profile_owner_microphone(p->owner,p->run,p->epoch,frame->sequence,frame->pcm,frame->bytes);
}
void dc_phone_runtime_close(struct dc_phone_runtime *p){
    if(!p||!p->owner)return;
    dc_phone_runtime_microphone_revoke(p);
    br_profile_owner_close(p->owner,p->run,p->epoch);
    if(p->terminal&&p->durable&&!p->done&&
       br_profile_owner_release(p->owner,p->run,p->epoch)==1)p->done=1;
}
int dc_phone_runtime_destroy(struct dc_phone_runtime *p){
    if(!p)return 0;
    /* UNKNOWN/ambiguous send retains its original source and owned command.
     * There is no reconnect, re-send or forged successful retirement. */
    if(p->owner&&br_profile_owner_free(p->owner))return -1;
    if(p->source.active&&eo_saved_close(&p->source))return -1;
    if(p->objects)eo_free(p->objects);if(p->store_open)br_store_close(&p->store);
    free(p->inbox);memset(p,0,sizeof(*p));free(p);return 0;
}
#endif
