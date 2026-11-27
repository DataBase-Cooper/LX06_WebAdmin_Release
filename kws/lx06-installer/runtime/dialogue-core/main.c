/* One session reactor for every provider. Only this executable subscribes to
 * processed microphone PCM and owns player/light/control policy. Platform
 * workers own transport only. The stock SDK remains an isolated TTS worker.
 * Never print user text, provider responses, URLs, headers or credentials. */
#define _GNU_SOURCE
#include "session.h"
#include "account.h"
#include "tts.h"
#include "protocol.h"
#include "audio_route.h"
#include "bootstrap.h"
#include "mode_gate.h"
#include "adapters/phone.h"
#include "media.h"
#include "voice_guard.h"
#include "asr/control_asr.h"
#include "io/io.h"
#include "io/compat.h"
#include "../doubao/src/text_match.h"
#include "../../hooks/xiaoai/session-owner.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define GREETING_STAT dc_io_stat
#define GREETING_FSTAT dc_io_fstat
#include "../common/script-greeting.h"

#define COMMANDS 8u
/* EX_TEMPFAIL (sysexits): reserved by this executable for capture overflow. */
#define DC_EXIT_CAPTURE_OVERFLOW 75
struct core {
    dc_session session;
    dc_account *account;dc_tts *tts;dc_bootstrap *bootstrap;dc_io_player *player;dc_io_alsa *alsa;
    dc_io_capture *capture;dc_io_lights *lights;dc_voice_guard *guard;
    dc_audio_route *route;
    lx06_control_asr *asr;
    dc_io_playback_backend real_backend;
    const char *provider,*script,*model;
    int audio,adapter_fd,ready,closing,failed,capture_overflow,player_live,player_sealed,stopping_player,audio_playback;
    int phone,action_result,action_started,cue_player;
#ifdef LX06_PHONE_PROFILE_RUNTIME
    struct dc_phone_runtime *phone_runtime;int phone_player_owned,phone_player_stopped;
#endif
    dc_mode_request mode_request;dc_phone_gate phone_gate;dc_media_cue cue;
#if defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) || defined(LX06_PHONE_PROFILE_RUNTIME)
    struct br_profile_owner *selected_owner;
    struct eo_saved_source *selected_source;
    const void *selected_caps;size_t selected_caps_size;
    uint64_t selected_run;int selected_result;
#endif
    pid_t adapter_pid;uint64_t adapter_kill_ms,started_ms,closed_ms;
    uint64_t synthesis_sequence,pcm_at_ms,synthetic_utterance;
    unsigned rate,backend_rate; /* reactor and player-worker ownership differ */
    int account_done,greeting,tts_ready;
    char greeting_text[241];
    int local_greeting, greeting_fd;
    unsigned greeting_rate, greeting_channels, greeting_bytes;
    off_t greeting_offset;
    dc_wire_packet commands[COMMANDS],pending_audio;
    unsigned command_head,command_count;int have_audio;
};
static volatile sig_atomic_t stopped;
static int trace_startup_enabled;
/* Optional fixed metadata only, emitted BEFORE each nominated startup call.
 * No user text, environment value, path, credential or account is printed. */
static void trace_startup(unsigned stage){
    static const char *const lines[]={"dialogue-core: startup_stage=1\n","dialogue-core: startup_stage=2\n","dialogue-core: startup_stage=3\n","dialogue-core: startup_stage=4\n","dialogue-core: startup_stage=5\n","dialogue-core: startup_stage=6\n","dialogue-core: startup_stage=7\n","dialogue-core: startup_stage=8\n","dialogue-core: startup_stage=9\n","dialogue-core: startup_stage=10\n"};
    if(!trace_startup_enabled||!stage||stage>10)return;
    const char *line=lines[stage-1];size_t left=strlen(line);
    while(left){ssize_t sent=write(STDERR_FILENO,line,left);if(sent<0&&errno==EINTR)continue;if(sent<=0)break;line+=sent;left-=(size_t)sent;}
}
static void stop_signal(int sig){(void)sig;stopped=1;}
static uint64_t now_ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000u+(unsigned long)t.tv_nsec;}
static uint64_t now_ms(void){return now_ns()/1000000u;}
static uint64_t wall_ms(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<0)return 0;return (uint64_t)t.tv_sec*1000u+(unsigned long)t.tv_nsec/1000000u;}
static int inherited_fd(const char *s){unsigned n=0;if(!s||!*s)return -1;for(;*s;s++){if(*s<'0'||*s>'9'||n>1024)return -1;n=n*10u+(unsigned)(*s-'0');}return n>=3&&n<=1024?(int)n:-1;}
static const char *env(const char *key,const char *fallback){const char *v=getenv(key);return v?v:fallback;}
static unsigned number(const char *name,unsigned fallback,unsigned low,unsigned high) {
    const char *v=getenv(name);char *end;unsigned long n;if(!v)return fallback;
    errno=0;n=strtoul(v,&end,10);return errno||!*v||*end||n<low||n>high?fallback:(unsigned)n;
}
static void note(const char *event,uint64_t value){fprintf(stderr,"dialogue-core: event=%s value=%llu at=%llu\n",event,(unsigned long long)value,(unsigned long long)now_ms());}
static int owned(void) {
#ifdef DC_CORE_HOST_FIXTURE
    if(!strcmp(env("DC_HOST_FIXTURE","0"),"1"))return 1;
#endif
    struct lx06_session_record r;dc_io_stat inherited;LX06_SESSION_STATBUF owner;
    return geteuid()==0&&lx06_session_identity(&r,NULL)&&r.magic==LX06_SESSION_MAGIC&&
        r.reserved>=3&&r.reserved<=1024&&r.group==(unsigned)getpgrp()&&
        r.group_ticks==lx06_process_ticks(r.group)&&r.supervisor_ticks==lx06_process_ticks(r.supervisor)&&
        !dc_io_fstat((int)r.reserved,&inherited)&&!LX06_SESSION_STAT(LX06_SESSION_FILE,&owner)&&
        owner.st_ino==inherited.st_ino&&owner.st_dev==inherited.st_dev;
}
static int command(struct core *c,unsigned type,uint64_t generation,uint64_t sequence,unsigned flags,const void *data,size_t bytes) {
    if(c->command_count==COMMANDS||bytes>DC_WIRE_CAP){c->failed=1;return -1;}
    dc_wire_packet *p=&c->commands[(c->command_head+c->command_count)%COMMANDS];
    p->header=dc_wire_make(type,c->session.control.tag.session_epoch,generation,sequence,bytes);p->header.flags=flags;
    if(bytes)memcpy(p->payload,data,bytes);
    c->command_count++;return 0;
}
static void flush_commands(struct core *c) {
    while(c->command_count&&c->adapter_fd>=0) {
        dc_wire_packet *p=&c->commands[c->command_head];
        if(dc_wire_send(c->adapter_fd,&p->header,p->payload)) {
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)c->failed=1;
            return;
        }
        c->command_head=(c->command_head+1)%COMMANDS;c->command_count--;
    }
}
static void stop_cue(struct core *c){
    dc_tag tag=dc_media_cue_stop(&c->cue);
    if(tag.session_epoch&&c->cue_player){
        int r=dc_io_player_stop(c->player,tag);
        if(!r)c->stopping_player=1;else if(r!=-ESTALE)c->failed=1;
        note("cue_stop_requested",0);
    }
    c->cue_player=0;
}
static void stop_local(void *context,dc_tag tag) {
    struct core *c=context;
#ifdef LX06_PHONE_PROFILE_RUNTIME
    /* Local authority is revoked before either original capture/player stop;
     * this does not send a packet or fabricate remote/whole-call cleanup. */
    if(c->phone_runtime&&dc_tag_equal(tag,c->session.control.tag)){dc_phone_runtime_microphone_revoke(c->phone_runtime);note("phone_mic_revoked",0);}
#endif
    /* No network operation occurs before this queue invalidation/eventfd. */
    if(c->player_live) {
        int status=dc_io_player_stop(c->player,tag);
        /* A rejected old owner must not alter the current owner's state. */
        if(status==-ESTALE)return;
        if(status){c->failed=1;return;}
        c->stopping_player=1;
    }
    if(!dc_media_cue_active(&c->cue)||c->cue.tag.session_epoch==tag.session_epoch)stop_cue(c);
    c->player_live=c->player_sealed=0;
    if(c->have_audio&&c->pending_audio.header.epoch==tag.session_epoch&&
#ifdef LX06_PHONE_PROFILE_RUNTIME
       (c->phone_runtime||c->pending_audio.header.generation==tag.reply_generation)
#else
       c->pending_audio.header.generation==tag.reply_generation
#endif
    )c->have_audio=0;
    note("stop_requested",tag.reply_generation);
}
static void cancel_remote(void *context,dc_tag tag) {
    struct core *c=context;uint64_t now=now_ms();
    if(c->tts)(void)dc_tts_cancel(c->tts,tag,now);
    if(c->account)(void)dc_account_cancel(c->account,tag,now);
    c->account_done=0;c->greeting=0;
    if(c->local_greeting) {
        if(c->greeting_fd>=0)close(c->greeting_fd);
        c->greeting_fd=-1;c->local_greeting=0;
        if(c->audio)dc_audio_route_local_greeting_done(c->route,tag);
    } else if(c->audio&&dc_audio_route_cancel(c->route,tag,now))c->failed=1;
    note("cancel_queued",tag.reply_generation);
}
static int question(void *context,dc_tag tag,const char *text) {
    struct core *c=context;c->synthesis_sequence=0;c->account_done=0;c->greeting=0;
    /* These are trusted metadata/result interfaces, not model text inputs. */
    if(c->phone||c->action_result)return -1;
    note("complete_question",tag.utterance_id);
    /* Opt-in acceptance observation exports only an expected fixed-phrase
     * match. Unknown user text, hashes and transcripts are never logged. */
    if(getenv("DC_EXPECT_QUESTION_1")) {
        unsigned matched=0;
        for(unsigned i=1;i<=3;i++){char key[32];snprintf(key,sizeof key,"DC_EXPECT_QUESTION_%u",i);const char *expected=getenv(key);if(expected&&!strcmp(text,expected))matched=i;}
        note("expected_question",matched);
    }
    if(!c->audio)return dc_account_start(c->account,tag,text,c->script,c->model,now_ms());
    /* Audio already reached the provider. The same complete FINAL commits
     * its stream; never send text as a second user turn. */
    return dc_audio_route_question(c->route,tag,now_ms());
}
static void close_remote(void *context) {
    struct core *c=context;c->closing=1;c->closed_ms=now_ms();
    if(c->tts)dc_tts_shutdown(c->tts,c->closed_ms);
    if(c->bootstrap)dc_bootstrap_stop(c->bootstrap,c->closed_ms);
    if(c->asr){lx06_control_asr_close(c->asr);c->asr=NULL;}
    if(c->capture){dc_io_capture_close(c->capture);c->capture=NULL;}
#ifdef LX06_PHONE_PROFILE_RUNTIME
    if(c->phone_runtime)dc_phone_runtime_close(c->phone_runtime);
#endif
    if(c->adapter_pid>0) {
        c->command_head=c->command_count=0;
        (void)command(c,DC_CMD_CLOSE,0,0,0,NULL,0);flush_commands(c);
        kill(-c->adapter_pid,SIGTERM);c->adapter_kill_ms=c->closed_ms+80;
    }
}
static void light(void *context,dc_tag tag,uint64_t sequence,enum dc_state state) {
    struct core *c=context;if(c->lights)(void)dc_io_lights_set(c->lights,tag,sequence,state);
    note("state",state);
}
/* The worker publishes accepted PCM only while the owner is still current. */
static int backend_open(void *ctx,unsigned rate){struct core *c=ctx;c->backend_rate=rate;return c->real_backend.open(c->real_backend.context,rate);}
static int backend_poll(void *ctx,struct pollfd *fds,unsigned cap){struct core *c=ctx;return c->real_backend.pollfds(c->real_backend.context,fds,cap);}
static ssize_t backend_write(void *ctx,const int16_t *pcm,size_t frames){struct core *c=ctx;return c->real_backend.write(c->real_backend.context,pcm,frames);}
static void player_reference_reset(void *ctx){struct core *c=ctx;dc_voice_guard_reset_reference(c->guard);}
static void player_reference_accepted(void *ctx,dc_tag tag,const int16_t *pcm,size_t frames,unsigned rate,uint64_t operation,uint64_t offset){struct core *c=ctx;dc_voice_guard_reference_span(c->guard,pcm,frames,rate,now_ms(),tag.session_epoch,tag.reply_generation,operation,offset);}
static int backend_delay(void *ctx,long *frames){struct core *c=ctx;return c->real_backend.delay(c->real_backend.context,frames);}
static int backend_prepare(void *ctx){struct core *c=ctx;return c->real_backend.prepare(c->real_backend.context);}
static int backend_drop(void *ctx){struct core *c=ctx;return c->real_backend.drop(c->real_backend.context);}
static void backend_close(void *ctx){struct core *c=ctx;c->real_backend.close(c->real_backend.context);}
static int begin_player(struct core *c,dc_tag tag,unsigned rate) {
    if(c->player_live)return c->rate==rate?0:-EPROTO;
    if(c->stopping_player)return -EAGAIN;
    int r=dc_io_player_begin(c->player,tag,rate);
    if(!r){c->player_live=1;c->player_sealed=0;c->rate=rate;}
    return r;
}
static int start_local_greeting(struct core *c,uint64_t now) {
    const char *name=getenv("LX06_SCRIPT_ID");
    if((!name||!*name)&&c->script) {
        const char *slash=strrchr(c->script,'/');
        name=slash?slash+1:c->script;
    }
    if(!name||!c->greeting_text[0]||c->session.control.next_generation)return 0;
    int fd=greeting_open(name,c->greeting_text,&c->greeting_rate,
                         &c->greeting_channels,&c->greeting_bytes);
    if(fd<0)return 0;
    if(!c->audio)(void)dc_session_ready(&c->session,now);
    dc_tag tag=dc_session_greeting(&c->session,c->greeting_text,now);
    if(!tag.session_epoch||(c->audio&&dc_audio_route_local_greeting(c->route,tag,now))) {
        close(fd);c->failed=1;return 1;
    }
    c->greeting_fd=fd;
    c->greeting_offset=GREETING_TEXT_BYTES+44;
    c->greeting=c->local_greeting=1;
    dc_session_spoken(&c->session,tag,c->greeting_text,now);
    note("local_greeting",tag.reply_generation);
    return 1;
}
static void pump_local_greeting(struct core *c,uint64_t now) {
    if(c->greeting_fd<0)return;
    dc_tag tag=c->session.control.tag;
    int r=begin_player(c,tag,c->greeting_rate);
    if(r==-EAGAIN)return;
    if(r){c->failed=1;return;}
    unsigned char pcm[DC_IO_PCM_WRITE_MAX];
    for(unsigned budget=0;budget<4&&c->greeting_bytes;budget++) {
        unsigned bytes=c->greeting_bytes<sizeof pcm?c->greeting_bytes:sizeof pcm;
        ssize_t n=pread(c->greeting_fd,pcm,bytes,c->greeting_offset);
        if(n<0&&errno==EINTR)return;
        if(n!=(ssize_t)bytes){c->failed=1;return;}
        if(c->greeting_channels==2)
            for(unsigned i=0;i<bytes/4;i++){pcm[i*2]=pcm[i*4];pcm[i*2+1]=pcm[i*4+1];}
        r=dc_io_player_write(c->player,tag,pcm,bytes/c->greeting_channels);
        if(r==-EAGAIN)return;
        if(r){c->failed=1;return;}
        c->greeting_offset+=bytes;c->greeting_bytes-=bytes;c->pcm_at_ms=now;
    }
    if(!c->greeting_bytes) {
        close(c->greeting_fd);c->greeting_fd=-1;
        if(dc_io_player_finish(c->player,tag)){c->failed=1;return;}
        c->player_sealed=1;c->account_done=1;
    }
}
static void send_microphone(struct core *c) {
    if(!c->audio||!c->ready||c->command_count||c->closing||c->failed)return;
    for(unsigned budget=0;budget<16;budget++) {
        dc_audio_action action;int next=dc_audio_route_next(c->route,&action);
        if(next<0){note("audio_route_error",(uint64_t)(-next));c->failed=1;return;}
        if(!next)break;
        unsigned type=action.type==DC_AUDIO_MIC?DC_CMD_MIC:action.type==DC_AUDIO_COMMIT?DC_CMD_RESUME:action.type==DC_AUDIO_GRANT?DC_CMD_GRANT:DC_CMD_CANCEL;
        dc_wire_header h=dc_wire_make(type,action.tag.session_epoch,action.tag.reply_generation,action.sequence,action.bytes);
        if(action.type==DC_AUDIO_MIC){h.flags=DC_WIRE_PCM_S16LE;h.rate=16000;h.channels=1;}
        else if(action.type==DC_AUDIO_COMMIT)h.flags=DC_WIRE_FINAL;
        if(dc_wire_send(c->adapter_fd,&h,action.pcm)) {
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)c->failed=1;
            return;
        }
        if(dc_audio_route_sent(c->route,&action,now_ms())){c->failed=1;return;}
    }
}
static void capture(struct core *c,uint64_t now) {
    for(unsigned budget=0;c->capture&&budget<8;budget++) {
        dc_io_capture_frame frame;int r=dc_io_capture_next(c->capture,&frame);if(r==0)break;
        if(r<0){c->failed=1;return;}
        if((frame.flags&DC_IO_CAPTURE_READY)&&!c->audio&&!c->phone&&!c->action_result)c->ready=1;
        if(frame.rate!=16000||(frame.bytes!=320&&frame.bytes!=640&&frame.bytes!=960)){c->failed=1;return;}
        int16_t pcm[480];dc_voice_result voice;
        if(dc_voice_guard_process(c->guard,(const int16_t *)frame.pcm,frame.bytes/2,c->session.control.config.gain_db,pcm,now,&voice)){c->failed=1;return;}
        lx06_control_asr_reply(c->asr,c->session.control.tag.reply_generation);
        int pending=0;
        if(ioctl(dc_io_capture_fd(c->capture),FIONREAD,&pending)||pending<0)pending=1;
        lx06_control_asr_mic_pending(c->asr,(size_t)pending);
        struct qev_stamp stamp={frame.producer_birth_ticks,frame.sequence,frame.available_mono_ms,frame.producer_pid,16000,frame.bytes/2,frame.origin};
        const struct qev_stamp *metadata=frame.mode==DC_IO_CAPTURE_LXA2&&!(frame.flags&DC_IO_CAPTURE_METADATA_INVALID)?&stamp:NULL;
        if(lx06_control_asr_feed_stamped(c->asr,1,pcm,frame.bytes,metadata,now)){c->failed=1;return;}
        /* Local voice evidence is keyed to the recognizer turn, never reused
         * to abort a later generation. Text controls use the same cancel path. */
        if(voice.near_end&&!voice.echo)lx06_control_asr_bind_source(c->asr);
        uint64_t utterance=lx06_control_asr_active_serial(c->asr);
        if(c->audio){
            int route_error=dc_audio_route_capture(c->route,pcm,frame.bytes,utterance,voice.near_end&&!voice.echo,now);
            if(route_error){note(route_error==-ENOBUFS?"capture_overflow":"capture_route_error",(uint64_t)(-route_error));c->capture_overflow=route_error==-ENOBUFS;c->failed=1;return;}
        }
        if(utterance&&voice.near_end) {
            uint64_t gen=0;(void)lx06_control_asr_partial(c->asr,&gen);
            (void)dc_session_voice(&c->session,(dc_tag){c->session.control.tag.session_epoch,gen,utterance},1,voice.echo,now);
        }
    }
    if(c->capture&&dc_io_capture_stalled(c->capture,now*1000000u)){note("capture_stalled",0);c->failed=1;}
}
static void recognition(struct core *c,uint64_t now) {
    if(!c->asr||c->closing)return;
    char text[DC_QUESTION_CAP];int result=lx06_control_asr_poll(c->asr,text,sizeof text,now);
    if(result<0){c->failed=1;return;}
    if(result>0) {
        dc_tag source={c->session.control.tag.session_epoch,lx06_control_asr_result_reply(c->asr),lx06_control_asr_result_serial(c->asr)};
        /* While connecting a phone, or announcing an already completed action,
         * only the shared control-word path is meaningful. Never turn metadata
         * or normal talk into a second business action/model question. */
        int interrupt=0;
        if(dc_session_text_interrupt(&c->session,source,text,!(c->phone||c->action_result),now,&interrupt)<0)c->failed=1;
        if(c->phone&&interrupt)stop_cue(c);
        if(c->route&&dc_audio_route_final(c->route,source.utterance_id))c->failed=1;
    }
    if(!c->asr)return;
    uint64_t gen=0;const char *partial=lx06_control_asr_partial(c->asr,&gen);
    uint64_t utterance=lx06_control_asr_partial_serial(c->asr);
    if(*partial&&utterance){
        /* A goodbye synchronously closes/frees ASR in close_remote. Consume
         * its borrowed partial before entering that shared control callback. */
        int interrupt=0;
        (void)dc_session_text_interrupt(&c->session,(dc_tag){c->session.control.tag.session_epoch,gen,utterance},partial,0,now,&interrupt);
        if(c->phone&&interrupt)stop_cue(c);
    }
}
static void player_events(struct core *c,uint64_t now) {
    dc_io_event e;
    for(unsigned budget=0;budget<16&&dc_io_player_next_event(c->player,&e)>0;budget++) {
        if(e.type==DC_IO_STOP_FAILED){note("player_drop_error",(unsigned)e.error);c->failed=1;continue;}
        if(e.type==DC_IO_STOPPED){
#ifdef LX06_PHONE_PROFILE_RUNTIME
            if(c->phone_runtime&&dc_tag_equal(e.tag,c->session.control.tag))c->phone_player_stopped=1;
#endif
            c->stopping_player=0;note("player_dropped",e.tag.reply_generation);continue;
        }
        if(e.tag.reply_generation==DC_MEDIA_CUE_GENERATION){
            if(e.type==DC_IO_ERROR)c->failed=1;
            else if(e.type==DC_IO_FIRST_PCM)note("cue_pcm",0);
            /* Audible connection cues retain CONNECTING, never SPEAKING or
             * LISTENING: actual PCM here does not establish phone call READY. */
            continue;
        }
        if(!dc_tag_equal(e.tag,c->session.control.tag))continue;
        if(e.type==DC_IO_ERROR){note("player_error",(unsigned)e.error);c->failed=1;}
        else if(e.type==DC_IO_FIRST_PCM||e.type==DC_IO_PCM_RESUMED){(void)dc_session_speaking(&c->session,e.tag,now);note(e.type==DC_IO_FIRST_PCM?"first_pcm":"pcm_resumed",e.tag.reply_generation);}
        else if(e.type==DC_IO_DRAINED) {
            c->player_live=c->player_sealed=0;
            if(c->local_greeting) {
                c->local_greeting=0;
                if(c->audio)dc_audio_route_local_greeting_done(c->route,e.tag);
            }
            if(c->account_done&&(!c->tts||!dc_tts_slots(c->tts))) {
                (void)dc_session_reply_done(&c->session,e.tag,now);c->greeting=0;
                if(c->audio&&dc_audio_route_played(c->route,e.tag)>0)(void)command(c,DC_CMD_RESUME,0,0,0,NULL,0);
            }
        }
    }
}
static void tts_bootstrap(struct core *c,uint64_t now) {
    if(!c->bootstrap)return;
    int ready=dc_bootstrap_tick(c->bootstrap,now);
    if(ready<0&&!c->closing){note("bootstrap_error",dc_bootstrap_error(c->bootstrap));c->failed=1;return;}
    if(ready>0&&!c->closing&&!c->tts) {
        dc_tts_config tc={dc_bootstrap_socket(c->bootstrap),env("LX06_NATIVE_TTS_DECODER","/usr/lib/lx06/native-url-audio"),dc_bootstrap_broker_pid(c->bootstrap)};
        c->tts=dc_tts_create(&tc);
        if(!c->tts||dc_tts_warm(c->tts,c->session.control.tag,now)){c->failed=1;return;}
    }
}
static void text_audio(struct core *c,uint64_t now) {
    if(c->audio||c->phone||c->closing)return;
    dc_account_event ae;
    for(unsigned budget=0;c->account&&budget<4&&dc_account_poll(c->account,now,&ae)>0;budget++) {
        if(!dc_tag_equal(ae.tag,c->session.control.tag))continue;
        if(ae.type==DC_ACCOUNT_ERROR){note("account_error",ae.error);c->failed=1;return;}
        if(ae.type==DC_ACCOUNT_DONE){c->account_done=1;note("model_done",ae.tag.reply_generation);}
        if(ae.type==DC_ACCOUNT_DELTA)note("model_delta_bytes",ae.length);
    }
    if(!c->tts)return;
    dc_tts_event te;
    for(unsigned budget=0;budget<8&&dc_tts_poll(c->tts,now,&te)>0;budget++) {
        if(te.type==DC_TTS_ERROR){note("tts_error",te.error);c->failed=1;return;}
        if(te.type==DC_TTS_SEGMENT_DONE)note("segment_done",te.segment_sequence);
        if(te.type==DC_TTS_WARM_READY) {
            c->tts_ready=1;
            if(!c->action_result)(void)dc_session_ready(&c->session,now);
            if(!c->session.control.next_generation&&c->greeting_text[0]) {
                dc_tag tag=dc_session_greeting(&c->session,c->greeting_text,now);c->greeting=1;c->account_done=1;
                if(c->action_result)c->action_started=1;
                if(!tag.session_epoch||dc_tts_submit(c->tts,tag,++c->synthesis_sequence,c->greeting_text,now)){c->failed=1;return;}
                dc_session_spoken(&c->session,tag,c->greeting_text,now);
            }
        }
    }
    if(c->account&&c->tts_ready&&c->session.control.reply_live&&!c->greeting) {
        while(dc_tts_slots(c->tts)<DC_TTS_SLOTS&&dc_account_phrase(c->account,now,&ae)>0) {
            if(!dc_tag_equal(ae.tag,c->session.control.tag))continue;
            if(dc_tts_submit(c->tts,ae.tag,++c->synthesis_sequence,ae.text,now)){c->failed=1;return;}
            dc_session_spoken(&c->session,ae.tag,ae.text,now);
        }
    }
    if(!dc_tts_decoder_active(c->tts)&&(!c->player_live||dc_io_player_queued(c->player)<(size_t)c->rate/5)) {
        dc_tag tag;uint64_t seq;int ready;
        if(dc_tts_next(c->tts,&tag,&seq,&ready)>0&&ready&&dc_tts_activate(c->tts,tag,seq,now)){c->failed=1;return;}
    }
    for(unsigned budget=0;budget<8;budget++) {
        dc_tts_pcm_view view;int available=dc_tts_peek_pcm(c->tts,&view);if(available<=0)break;
        if(!dc_tag_equal(view.tag,c->session.control.tag)){c->failed=1;return;}
        int r=begin_player(c,view.tag,view.rate);if(r==-EAGAIN)break;if(r){c->failed=1;return;}
        r=dc_io_player_write(c->player,view.tag,view.data,view.bytes);if(r==-EAGAIN)break;if(r){c->failed=1;return;}
        c->pcm_at_ms=now;(void)dc_tts_consume_pcm(c->tts,view.tag,view.segment_sequence,view.bytes);
    }
    if(!c->local_greeting&&c->session.control.reply_live&&c->account_done&&(c->greeting||(c->account&&dc_account_drained(c->account)))&&!dc_tts_slots(c->tts)) {
        if(c->player_live&&!c->player_sealed){(void)dc_io_player_finish(c->player,c->session.control.tag);c->player_sealed=1;}
        else if(!c->player_live&&!c->stopping_player)(void)dc_session_reply_done(&c->session,c->session.control.tag,now);
    }
    /* A segment gap is THINKING, never END; next PCM restores speaking. */
    if(c->session.control.reply_live&&c->session.control.state==DC_SPEAKING&&now>c->pcm_at_ms+250&&dc_io_player_queued(c->player)==0)
        (void)dc_session_waiting(&c->session,c->session.control.tag,now);
}
static int adapter_start(struct core *c) {
#if defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) || defined(LX06_PHONE_PROFILE_RUNTIME)
    if(c->selected_owner){
        c->selected_result=dc_phone_selected_start(c->selected_owner,c->selected_source,
            &c->mode_request,c->selected_run,c->session.control.tag.session_epoch,
            c->selected_caps,c->selected_caps_size);
        return c->selected_result==1?0:-1;
    }
#endif
    int sockets[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,sockets))return -1;
    const char *path=c->phone?env("DC_PHONE_WORKER","/usr/lib/lx06/lx06-phone-worker"):env("DC_TRANSPORT_ADAPTER","/usr/lib/lx06/lx06-transport-adapter");
    if(!c->phone&&setenv("DC_UPLINK_MODE",c->session.control.config.barge_any?"native":"guarded",1)){close(sockets[0]);close(sockets[1]);return -1;}
    int limit=dc_mode_child_limit(),sink=open("/dev/null",O_RDWR|O_CLOEXEC);
    if(limit<0||sink<0){if(sink>=0)close(sink);close(sockets[0]);close(sockets[1]);return -1;}
    char *const args_audio[]={(char *)path,"--provider",(char *)c->provider,NULL};
    char *const args_phone[]={(char *)path,NULL};extern char **environ;char **child_env=environ;
    pid_t parent=getpid(),pid=fork();if(pid<0){close(sink);close(sockets[0]);close(sockets[1]);return -1;}
    if(!pid) {
        signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGPIPE,SIG_DFL);
        if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(126);
        if(dc_mode_child_isolate(sockets[1],DC_ADAPTER_FD,sink,limit))_exit(126);
        execve(path,c->phone?args_phone:args_audio,child_env);_exit(127);
    }
    close(sink);close(sockets[1]);setpgid(pid,pid);c->adapter_fd=sockets[0];c->adapter_pid=pid;
    if(c->phone){
        char envelope[1024];int n=dc_mode_phone_open(&c->mode_request,c->session.control.tag.session_epoch,wall_ms(),now_ms(),&c->phone_gate,envelope,sizeof envelope);
        return n<0?-1:command(c,DC_CMD_OPEN,0,0,0,envelope,(size_t)n);
    }
    return command(c,DC_CMD_OPEN,0,0,0,NULL,0);
}
int dc_main_selected_start(struct br_profile_owner *owner,struct eo_saved_source *source,
        const dc_mode_request *request,uint64_t run,uint64_t epoch,const void *open,size_t n) {
#if defined(LX06_SELECTED_SCRIPT_PROFILE_FIXTURE) || defined(LX06_PHONE_PROFILE_RUNTIME)
    /* Execute the actual adapter constructor without starting audio/reactor
     * resources. The normal main has no registered A source until T10/T14.
     * Identity is supplied by its original owner, never allocated here. This
     * entry cannot create a selection from request text: the source's native
     * dispatch must already be matched and bound to that same identity. */
#ifndef LX06_SELECTED_SCRIPT_PROFILE_FIXTURE
    if(!owned())return -1;
#endif
    if(!owner)return -1;
    struct core *c=calloc(1,sizeof(*c));if(!c)return -1;
    c->phone=1;c->selected_owner=owner;c->selected_source=source;
    c->selected_run=run;c->session.control.tag.session_epoch=epoch;
    c->selected_caps=open;c->selected_caps_size=n;
    if(request)c->mode_request=*request;
    (void)adapter_start(c);int rc=c->selected_result;free(c);return rc;
#else
    (void)owner;(void)source;(void)request;(void)run;(void)epoch;(void)open;(void)n;
    return -1; /* Production registration remains refused. */
#endif
}
static void connection_cue(struct core *c){
    if(!c->phone||c->closing||!dc_media_cue_active(&c->cue)||c->stopping_player)return;
    if(!c->cue_player){if(dc_io_player_begin(c->player,c->cue.tag,DC_MEDIA_RATE)){c->failed=1;return;}c->cue_player=1;}
    for(unsigned budget=0;budget<3;budget++){
        dc_media_frame f;int r=dc_media_cue_next(&c->cue,dc_io_player_queued(c->player)/sizeof(int16_t),&f);
        if(r==0||r==-EAGAIN)return;
        if(r<0){c->failed=1;return;}
        r=dc_io_player_write(c->player,f.tag,f.pcm,f.samples*sizeof f.pcm[0]);if(r){c->failed=1;return;}
    }
}
#ifdef LX06_PHONE_PROFILE_RUNTIME
static int phone_audio_accept(void *ctx,const dc_wire_packet *packet){
    struct core *c=ctx;
    if(!c||!packet||c->closing||c->failed||c->have_audio||
       packet->header.epoch!=c->session.control.tag.session_epoch)return -1;
    /* Borrow the core's existing single pending slot. It is not a second
     * player queue. The profile advances at most one message per reactor tick. */
    c->pending_audio=*packet;c->have_audio=1;return 1;
}
static int phone_media_reaped(void *ctx,uint64_t run,uint64_t epoch){
    struct core *c=ctx;
    return c&&run==epoch&&epoch==c->session.control.tag.session_epoch&&c->closing&&
        !c->have_audio&&!c->player_live&&!c->stopping_player&&!dc_io_player_queued(c->player)&&
        (!c->phone_player_owned||c->phone_player_stopped)&&
        !c->capture&&!c->asr&&!c->adapter_pid&&c->adapter_fd<0;
}
static void phone_microphone(struct core *c,uint64_t now){
    if(!c||!c->phone_runtime||c->closing||c->failed)return;
    int ready=dc_phone_runtime_microphone_allowed(c->phone_runtime);
    if(ready<0){c->failed=1;return;}
    if(ready!=1){if(c->capture)c->failed=1;return;}
    if(!c->capture){
        c->capture=dc_io_capture_open_mode(env("DC_MIC_DIRECTORY","/tmp/lx06-mic"),"account",
            !strcmp(env("DC_MIC_FORMAT","LXA2"),"LXA1")?DC_IO_CAPTURE_LXA1:DC_IO_CAPTURE_LXA2);
        if(!c->capture){c->failed=1;return;}note("phone_mic_opened",0);
    }
    for(unsigned budget=0;c->capture&&budget<8;budget++){
        /* Recheck source and the actual started/confirmed phase before PCM. */
        if(stopped||dc_phone_runtime_microphone_allowed(c->phone_runtime)!=1){c->failed=1;return;}
        dc_io_capture_frame frame;int rc=dc_io_capture_next(c->capture,&frame);
        if(!rc)break;if(rc<0){c->failed=1;return;}
        rc=dc_phone_runtime_microphone(c->phone_runtime,&frame);
        memset(&frame,0,sizeof frame);if(rc<0){c->failed=1;return;}
    }
    if(c->capture&&dc_io_capture_stalled(c->capture,now*1000000u)){note("capture_stalled",0);c->failed=1;}
}
static void phone_audio(struct core *c,uint64_t now){
    if(!c->have_audio||c->closing||c->failed)return;
    dc_wire_packet *p=&c->pending_audio;
    /* Wire generation0 is the retained remote operation. Allocate playback
     * only through the original core policy; never overwrite either identity. */
    if(!c->session.control.reply_live){
        (void)dc_session_ready(&c->session,now);
        if(!dc_session_reply(&c->session,++c->synthetic_utterance,now).session_epoch){c->failed=1;return;}
    }
    dc_tag tag=c->session.control.tag;int status=begin_player(c,tag,p->header.rate);
    if(status==-EAGAIN)return;
    if(status){c->failed=1;return;}
    c->phone_player_owned=1;c->phone_player_stopped=0;
    size_t offset=0;
    while(offset<p->header.length){
        size_t n=p->header.length-offset;if(n>DC_IO_PCM_WRITE_MAX)n=DC_IO_PCM_WRITE_MAX;
        status=dc_io_player_write(c->player,tag,p->payload+offset,n);
        if(status==-EAGAIN){memmove(p->payload,p->payload+offset,p->header.length-offset);p->header.length-=(uint32_t)offset;return;}
        if(status){c->failed=1;return;}offset+=n;c->pcm_at_ms=now;
    }
    c->have_audio=0;
}
#endif
static void adapter_audio(struct core *c,dc_wire_packet *p,uint64_t now) {
    dc_tag tag=c->session.control.tag;
    if(p->header.epoch!=tag.session_epoch||p->header.generation!=tag.reply_generation||!c->session.control.reply_live||!dc_audio_route_accepts(c->route,p->header.sequence,p->header.generation)){c->have_audio=0;return;}
    if(p->header.channels!=1){c->failed=1;return;}
    if(!c->audio_playback){c->have_audio=0;return;}
    int r=begin_player(c,tag,p->header.rate);if(r==-EAGAIN){c->have_audio=1;return;}if(r){c->failed=1;return;}
    size_t offset=0;
    while(offset<p->header.length) {
        size_t n=p->header.length-offset;if(n>DC_IO_PCM_WRITE_MAX)n=DC_IO_PCM_WRITE_MAX;
        r=dc_io_player_write(c->player,tag,p->payload+offset,n);
        if(r==-EAGAIN){memmove(p->payload,p->payload+offset,p->header.length-offset);p->header.length-=(uint32_t)offset;c->have_audio=1;return;}
        if(r){c->failed=1;return;}offset+=n;c->pcm_at_ms=now;
    }
    c->have_audio=0;
}
static void adapter_events(struct core *c,uint64_t now) {
    if((!c->audio&&!c->phone)||c->closing||c->failed)return;
    if(c->have_audio){adapter_audio(c,&c->pending_audio,now);if(c->have_audio)return;}
    for(unsigned budget=0;budget<16;budget++) {
        dc_wire_packet p;int r=dc_wire_receive(c->adapter_fd,&p);
        if(r<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)c->failed=1;break;}
        if(!r){c->failed=1;break;}
        if(p.header.epoch!=c->session.control.tag.session_epoch){c->failed=1;break;}
        if(c->phone){
            int accepted=dc_mode_phone_event(&c->phone_gate,&p,now);
            if(accepted<0){stop_cue(c);note("phone_refused",0);c->failed=1;break;}
            if(accepted>0){
                /* Current phone worker explicitly refuses RESUME: there is no
                 * independently verifiable remote turn fence. Even a complete
                 * metadata handshake cannot safely enable the audio route. */
                stop_cue(c);note("phone_media_unavailable",0);c->failed=1;break;
            }
            note("phone_metadata",0);continue;
        }
        switch(p.header.type) {
        case DC_EVT_READY:
            if(c->ready)break;
            c->ready=1;(void)dc_audio_route_ready(c->route);(void)dc_session_ready(&c->session,now);
            /* A complete user question received during connect takes priority.
             * Otherwise the greeting gets its own cancellable cloud request;
             * it must never steal the first QUESTION's generation/PCM. */
            if(!c->session.control.next_generation&&c->greeting_text[0]&&
               !(c->audio_playback&&start_local_greeting(c,now))) {
                dc_tag tag=dc_session_greeting(&c->session,c->greeting_text,now);
                if(!tag.session_epoch||dc_audio_route_greeting(c->route,tag,now)||command(c,DC_CMD_GREET,tag.reply_generation,0,0,c->greeting_text,strlen(c->greeting_text))){c->failed=1;break;}
                c->greeting=1;
            }
            (void)command(c,DC_CMD_RESUME,0,0,0,NULL,0);break;
        case DC_EVT_REPLY_BEGIN:{
            dc_tag tag;int begun=dc_audio_route_begin(c->route,p.header.sequence,now,&tag);
            if(begun<0){note("audio_begin_error",(uint64_t)(-begun));c->failed=1;break;}
            if(begun==DC_AUDIO_BEGIN_STALE||begun==DC_AUDIO_BEGIN_CANCELLED)break;
            if(begun==DC_AUDIO_BEGIN_NEED_TAG){
                /* Native providers may reply before a local transcript. Only
                 * this core can allocate that reply's playback generation. */
                if(c->session.control.reply_live){c->failed=1;break;}
                tag=dc_session_reply(&c->session,++c->synthetic_utterance,now);
                if(!tag.session_epoch||dc_audio_route_bind(c->route,p.header.sequence,tag,now)){c->failed=1;break;}
            }
            if(!dc_tag_equal(tag,c->session.control.tag)||!c->session.control.reply_live){c->failed=1;break;}
            c->account_done=0;break;
        }
        case DC_EVT_AUDIO:c->pending_audio=p;adapter_audio(c,&c->pending_audio,now);if(c->have_audio)return;break;
        case DC_EVT_TEXT:
            if(p.header.generation==c->session.control.tag.reply_generation&&dc_audio_route_accepts(c->route,p.header.sequence,p.header.generation)&&p.header.length<1441) {
                char text[1441];memcpy(text,p.payload,p.header.length);text[p.header.length]=0;
                if(c->audio_playback)dc_session_spoken(&c->session,c->session.control.tag,text,now);
            }break;
        case DC_EVT_REPLY_END:
            if(p.header.generation==c->session.control.tag.reply_generation&&dc_audio_route_end(c->route,p.header.sequence,p.header.generation)>0) {
                c->account_done=1;
                if(c->player_live){(void)dc_io_player_finish(c->player,c->session.control.tag);c->player_sealed=1;}
                else {
                    if(dc_audio_route_played(c->route,c->session.control.tag)>0)(void)command(c,DC_CMD_RESUME,0,0,0,NULL,0);
                    (void)dc_session_reply_done(&c->session,c->session.control.tag,now);
                }
            }break;
        case DC_EVT_RESUMED:if(dc_audio_route_resumed(c->route,p.header.sequence,p.header.generation)>0)note("uplink_resumed",p.header.sequence);break;
        case DC_EVT_CANCEL_ACK:case DC_EVT_USER_PARTIAL:case DC_EVT_USER_FINAL:break;
        case DC_EVT_ERROR:c->failed=1;break;
        default:c->failed=1;break;
        }
    }
}
static void reap_adapter(struct core *c,uint64_t now) {
    if(c->adapter_pid<=0)return;
    if(c->closing&&now>=c->adapter_kill_ms)kill(-c->adapter_pid,SIGKILL);
    int status;pid_t result=waitpid(c->adapter_pid,&status,WNOHANG);
    if(result==c->adapter_pid){c->adapter_pid=0;if(c->adapter_fd>=0)close(c->adapter_fd);c->adapter_fd=-1;if(!c->closing)c->failed=1;}
}
static int configure(dc_config *config,const char *prefix) {
    char key[80];memset(config,0,sizeof *config);
#define KEY(suffix) (snprintf(key,sizeof key,"%s_%s",prefix,suffix),key)
    int xz=!strcmp(prefix,"XZ");
    const char *interrupt=xz?env("XZ_INTERRUPT_TEXT","等一下,等等,停一下,你说的不对,你说得不对"):env(KEY("INTERRUPT"),"等一下,等等,停一下,你说的不对,你说得不对");
    const char *goodbye=xz?env("XZ_GOODBYE_TEXT","小智再见,再见小智,拜拜了哦"):env(KEY("GOODBYE"),"结束对话,拜拜了哦");
    if(strlen(interrupt)>=sizeof config->interrupt||strlen(goodbye)>=sizeof config->goodbye)return -1;
    strcpy(config->interrupt,interrupt);strcpy(config->goodbye,goodbye);
    config->barge_any=number(KEY("BARGE_ANY"),0,0,1);config->idle_ms=number(KEY("IDLE_SECONDS"),120,1,3600)*1000;
    config->tail_ms=number(KEY("TAIL_MS"),300,0,5000);
    if(xz){config->idle_ms=number("XZ_IDLE_TIMEOUT_MS",120000,0,86400000);config->tail_ms=number("XZ_PLAYBACK_TAIL_MUTE_MS",500,0,5000);}
    const char *gain=xz?env("XZ_INPUT_GAIN_DB","0"):env(KEY("MIC_GAIN_DB"),"0");char *end;config->gain_db=strtod(gain,&end);
#undef KEY
    return !*gain||*end||!isfinite(config->gain_db)||config->gain_db< -24||config->gain_db>24?-1:0;
}
/* fd < 0 means legacy settings; an opted-in fd must bind successfully.
 * The returned declaration is derived from the runner's own sealed bytes. */
static int configure_account(struct core *c,dc_config *config,uint64_t epoch,int fd) {
    c->account=dc_account_create();
    if(!c->account)return -1;
    int mode=fd<0?DC_ACCOUNT_LEGACY:dc_account_bind_sealed_v1(c->account,epoch,fd);
    if(mode<0 || configure(config,"MODEL"))goto fail;
    if(mode==DC_ACCOUNT_KEYWORD)config->barge_any=0;
    else if(mode==DC_ACCOUNT_VOICE)config->barge_any=1;
    else if(mode!=DC_ACCOUNT_LEGACY)goto fail;
    return 0;
fail:
    (void)dc_account_destroy(c->account);c->account=NULL;return -1;
}
int main(int argc,char **argv) {
    trace_startup_enabled=!strcmp(env("DC_TRACE_STARTUP","0"),"1");trace_startup(1);
    if(argc==2&&!strcmp(argv[1],"--protocol")){puts("LX06DIALOGUE/1");return 0;}
    int audio=argc>=3&&!strcmp(argv[1],"--audio");
    int phone=argc==3&&!strcmp(argv[1],"--phone-fd"),action_result=argc==3&&!strcmp(argv[1],"--action-result-fd");
    if((!audio&&!phone&&!action_result&&(argc<4||strcmp(argv[1],"--account")))||(audio&&strcmp(argv[2],"xiaozhi")&&strcmp(argv[2],"doubao")))return 2;
    trace_startup(2);if(!owned()){note("ownership_refused",0);return 77;}
    dc_mode_request request={0};
    if(phone||action_result){
        int fd=inherited_fd(argv[2]);int invalid=fd<0||dc_mode_read_request(fd,phone?DC_MODE_PHONE:DC_MODE_ACTION_RESULT,&request);
        if(fd>=3)close(fd);
        if(invalid){note("mode_input_refused",0);return 2;}
    }
    signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);signal(SIGHUP,stop_signal);signal(SIGPIPE,SIG_IGN);
    if(prctl(PR_SET_CHILD_SUBREAPER,1)){note("subreaper_refused",0);return 1;}
    struct core *c=calloc(1,sizeof *c);if(!c)return 1;c->audio=audio;c->phone=phone;c->action_result=action_result;c->mode_request=request;c->adapter_fd=-1;c->greeting_fd=-1;
    c->provider=phone?"phone":audio?argv[2]:"account";c->script=phone||action_result?c->mode_request.script:audio?NULL:argv[2];c->model=audio||phone||action_result?NULL:argv[3];
    const char *prefix=phone?"PHONE":action_result?"ACTION":audio?(!strcmp(c->provider,"xiaozhi")?"XZ":"DB"):"MODEL";
    uint64_t epoch=now_ns();
    const char *sealed=getenv("DC_ACCOUNT_SEALED_FD");
    int account_mode=!audio&&!phone&&!action_result;
    int sealed_fd=sealed?inherited_fd(sealed):-1;
    if(sealed&&(!account_mode||sealed_fd<0)){free(c);return 2;}
    trace_startup(3);dc_config config;
    if((account_mode?configure_account(c,&config,epoch,sealed_fd):configure(&config,prefix))){free(c);return 2;}
    if(!dc_config_valid(&config)){
        note("config_refused",0);
        if(c->account)(void)dc_account_destroy(c->account);
        free(c);return 2;
    }
    trace_startup(4);c->guard=dc_voice_guard_create();
    trace_startup(5);c->alsa=dc_io_alsa_create(env("DC_PCM_DEVICE","notify"),&c->real_backend);
    dc_io_playback_backend wrapped={c,backend_open,backend_poll,backend_write,backend_delay,backend_prepare,backend_drop,backend_close};
    dc_io_reference reference={c,player_reference_reset,player_reference_accepted};
    trace_startup(6);if(c->alsa&&c->guard)c->player=dc_io_player_create_with_reference(&wrapped,&reference);
    trace_startup(7);const char *light_fd=getenv("LX06_DIALOG_FD");if(light_fd)c->lights=dc_io_lights_create(atoi(light_fd),getenv("LX06_DIALOG_CHANNEL"));
    dc_session_ops ops={c,stop_local,cancel_remote,question,close_remote,light};
    c->started_ms=now_ms();
    trace_startup(8);if(!c->guard||!c->player){c->failed=1;goto cleanup;}
#ifdef LX06_PHONE_PROFILE_RUNTIME
    if(phone){
        /* Same normal owner, policy, fixed player/ALSA and pending slot. The
         * original capture opens only after the correlated started GRANT for
         * explicit MIC. Setup uplink is not READY or downlink permission; the
         * eventual READY must independently confirm the complete vector. */
        dc_phone_media_io media={c,phone_audio_accept,phone_media_reaped};
        c->phone_runtime=dc_phone_runtime_open(&request,epoch,epoch,&media);
        /* The base configuration was validated before opening the runtime.
         * Consume only its current retained word override before the one
         * original dc_session copies configuration; no live policy mutation. */
        if(!c->phone_runtime||dc_phone_runtime_configure(c->phone_runtime,&config)){
            c->failed=1;goto cleanup;
        }
    }
#endif
    if(dc_session_open(&c->session,epoch,&config,&ops,phone||action_result?c->mode_request.wake:audio?(argc>3?argv[3]:""):(argc>4?argv[4]:""),c->started_ms)){c->failed=1;goto cleanup;}
#ifdef LX06_PHONE_PROFILE_RUNTIME
    if(phone)goto run;
#endif
    trace_startup(9);c->asr=lx06_control_asr_open_profile(getenv("LX06_CONTROL_ASR_SOCKET"),LX06_ASR_QUESTION);
    trace_startup(10);c->capture=dc_io_capture_open_mode(env("DC_MIC_DIRECTORY","/tmp/lx06-mic"),"account",!strcmp(env("DC_MIC_FORMAT","LXA2"),"LXA1")?DC_IO_CAPTURE_LXA1:DC_IO_CAPTURE_LXA2);
    if(!c->asr||!c->capture){c->failed=1;goto run;}
    if(phone){
        if(adapter_start(c)||dc_media_cue_begin(&c->cue,epoch,NULL)){c->failed=1;goto run;}
    }
    else if(audio){
        const char *playback=env(!strcmp(prefix,"XZ")?"XZ_PLAYBACK":"DB_PLAYBACK","1");
        if(strcmp(playback,"0")&&strcmp(playback,"1")){c->failed=1;goto run;}c->audio_playback=!strcmp(playback,"1");
        const char *greeting=env(!strcmp(prefix,"XZ")?"XZ_WAKE_TEXT":"DB_GREETING",!strcmp(prefix,"XZ")?"小爱同学召唤小智":"我在呢，请说。");
        if(strlen(greeting)>=sizeof c->greeting_text){c->failed=1;goto run;}strcpy(c->greeting_text,greeting);
        c->route=dc_audio_route_create(epoch,config.barge_any);if(!c->route||adapter_start(c)){c->failed=1;goto run;}
    }
    else {
        dc_bootstrap_config bc={.broker_path=env("DC_TTS_BROKER","/usr/lib/lx06/native-tts-broker"),
            .worker_path=env("DC_TTS_WORKER","/usr/lib/lx06/native-tts-session-worker"),
            .snapshot_client_path=env("DC_TTS_SNAPSHOT_CLIENT","/usr/lib/lx06/native-config-snapshot-client"),
            .sdk_path=env("LX06_NATIVE_TTS_SDK","/usr/lib/libaivs_sdk.so"),.mico_comm="mico_aivs_lab"};
        c->bootstrap=dc_bootstrap_create(epoch,&bc,now_ms());if((!action_result&&!c->account)||!c->bootstrap){c->failed=1;goto run;}
        const char *greeting=action_result?dc_mode_action_text(&c->mode_request):env("MODEL_GREETING","我在呢，请说。");
        if(strlen(greeting)>=sizeof c->greeting_text){c->failed=1;goto run;}
        strcpy(c->greeting_text,greeting);
        if(account_mode)(void)start_local_greeting(c,now_ms());
    }
run:
    while(
#ifdef LX06_PHONE_PROFILE_RUNTIME
          c->phone_runtime||
#endif
          c->session.control.state!=DC_CLOSED) {
        uint64_t now=now_ms();
#ifdef LX06_PHONE_PROFILE_RUNTIME
        if(c->phone_runtime){
            if((stopped||c->failed||now-c->started_ms>number("DC_MAX_SECONDS",900,1,3600)*1000u)&&!c->closing)
                (void)dc_session_close(&c->session,now);
            /* Same STOP-first session callback and original fixed worker.
             * Do not consume another TLS frame while its pending PCM slot is
             * under player backpressure. No audio callback runs during close. */
            player_events(c,now);
            if(!c->closing&&!c->failed&&c->have_audio)phone_audio(c,now);
            if(c->failed&&!c->closing)(void)dc_session_close(&c->session,now);
            int status=(!c->have_audio||c->closing||c->failed)?dc_phone_runtime_tick(c->phone_runtime,now):0;
            if(status<0)c->failed=1;
            if(!status&&!c->closing&&!c->failed)phone_microphone(c,now);
            if((status||c->failed)&&!c->closing)(void)dc_session_close(&c->session,now);
            if(c->closing&&!c->stopping_player&&status&&status!=2){
                (void)dc_session_finish_close(&c->session,now);break;
            }
            if(c->closing&&now>=c->closed_ms&&now-c->closed_ms>2000){note("cleanup_timeout",0);c->failed=1;break;}
            struct pollfd media_wait[2]={{dc_io_player_event_fd(c->player),POLLIN,0},
                {c->capture?dc_io_capture_fd(c->capture):-1,POLLIN,0}};
            (void)poll(media_wait,2,5);continue;
        }
#endif
        if((stopped||c->failed||now-c->started_ms>number("DC_MAX_SECONDS",900,1,3600)*1000u)&&!c->closing)(void)dc_session_close(&c->session,now);
        /* STOP/control always run before model/TTS/network work. Each stage
         * has a bounded batch; no loop drains an arbitrary network backlog. */
        player_events(c,now);
        if(!c->closing&&!c->failed){capture(c,now);if(!c->failed)recognition(c,now);}
        if(c->account)dc_account_tick(c->account,now);
        if(c->tts)dc_tts_tick(c->tts,now);
        tts_bootstrap(c,now);
        reap_adapter(c,now);if(!c->failed||c->closing)flush_commands(c);
        if(c->phone&&!c->closing&&!c->failed&&dc_mode_phone_tick(&c->phone_gate,now)){stop_cue(c);note("phone_connect_timeout",0);c->failed=1;}
        if(!c->failed)connection_cue(c);
        if(!c->closing&&!c->failed)pump_local_greeting(c,now);
        if(!c->closing&&!c->failed){send_microphone(c);if(!c->failed)adapter_events(c,now);if(!c->failed)text_audio(c,now);if(!c->failed)(void)dc_session_tick(&c->session,now);if(!c->failed&&c->route&&dc_audio_route_tick(c->route,now))c->failed=1;}
        if(c->action_result&&c->action_started&&!c->closing&&!c->session.control.reply_live&&!c->stopping_player)(void)dc_session_close(&c->session,now);
        if(c->closing&&!c->stopping_player&&!c->adapter_pid&&(!c->account||(!dc_account_busy(c->account)&&!dc_account_retired(c->account)))&&(!c->tts||dc_tts_quiescent(c->tts))&&(!c->bootstrap||dc_bootstrap_quiescent(c->bootstrap))) {
            (void)dc_session_finish_close(&c->session,now);break;
        }
        if(c->closing&&now>=c->closed_ms&&now-c->closed_ms>2000){note("cleanup_timeout",0);c->failed=1;break;}
        struct pollfd waits[4]={{c->capture?dc_io_capture_fd(c->capture):-1,POLLIN,0},{dc_io_player_event_fd(c->player),POLLIN,0},{c->adapter_fd,POLLIN,0},{c->account?dc_account_fd(c->account):-1,POLLIN,0}};
        (void)poll(waits,4,5);
    }
cleanup:
    if(c->session.control.tag.session_epoch&&!c->closing)(void)dc_session_close(&c->session,now_ms());
#ifdef LX06_PHONE_PROFILE_RUNTIME
    if(c->phone_runtime){
        dc_phone_runtime_close(c->phone_runtime);
        if(dc_phone_runtime_destroy(c->phone_runtime))c->failed=1;
        c->phone_runtime=NULL;
    }
#endif
    if(c->asr)lx06_control_asr_close(c->asr);
    if(c->capture)dc_io_capture_close(c->capture);
    if(c->greeting_fd>=0)close(c->greeting_fd);
    dc_io_player_destroy(c->player);dc_io_alsa_destroy(c->alsa);dc_voice_guard_destroy(c->guard);dc_audio_route_destroy(c->route);
    if(c->account)(void)dc_account_destroy(c->account);
    if(c->tts)(void)dc_tts_destroy(c->tts);
    if(c->bootstrap)(void)dc_bootstrap_destroy(c->bootstrap);
    dc_io_lights_destroy(c->lights);
    /* session-run retains the lease and final light until this process and
     * all its descendants are actually reaped, even on a failed cleanup. */
    /* 75 is the core's capture-overflow status, meaningful only when the
     * directly observed child is this core, not an arbitrary script. */
    int result=c->capture_overflow?DC_EXIT_CAPTURE_OVERFLOW:c->failed?1:0;free(c);return result;
}
