#define _POSIX_C_SOURCE 200809L
/* LX06 Doubao realtime client. Public source contains no personal API key.
 * DB_BARGE_ANY=1 keeps server-native speech interruption. Mode 0 protects the
 * main stream while shared Xiaomi ASR recognizes explicit control words.
 * A cancellation belongs to one reply: duplicate words/late acknowledgements
 * must neither disconnect the session nor stop its next reply. Audio captured
 * during cancellation cleanup is bounded and replayed at the original pace.
 * response.done must also release protection for empty/cancelled responses;
 * output_audio.done is not guaranteed. See docs/DEVELOPMENT.md at repo root.
 */
#include "ws.h"
#include "json.h"
#include "player.h"
#include "ogg_audio.h"
#include "text_match.h"
#include "../../common/control_asr.h"
#include "../../../hooks/xiaoai/dialog-light.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int n) { stopping=n; }
static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static const char *env(const char *key,const char *fallback) {
    const char *s=getenv(key); return s && *s ? s : fallback;
}
static long number(const char *key,long fallback,long low,long high) {
    const char *s=getenv(key); char *end; if (!s || !*s) return fallback;
    long v=strtol(s,&end,10); return *end || v<low || v>high ? fallback : v;
}
static int quote(const char *s,char *out,size_t cap) {
    size_t n=0;
    for (;*s;s++) {
        /* Extension textareas can contain newlines. Escape JSON controls as
         * data instead of rejecting a valid user prompt during session setup. */
        unsigned char c=(unsigned char)*s;
        size_t need=c<32?6:((c=='"'||c=='\\')?2:1);
        if(n+need>=cap)return -1;
        if(c<32){snprintf(out+n,cap-n,"\\u%04x",c);n+=6;}
        else {if(need==2)out[n++]='\\';out[n++]=*s;}
    }
    out[n]=0; return 0;
}
static int simple(xz_ws *ws,const char *type) {
    char buf[128]; snprintf(buf,sizeof(buf),"{\"type\":\"%s\"}",type);
    return xz_ws_send_text(ws,buf);
}
static int speech(xz_ws *ws,const char *s) {
    char escaped[1024],body[1152]; if (quote(s,escaped,sizeof(escaped))) return -1;
    snprintf(body,sizeof(body),"{\"type\":\"speech_text_buffer.commit\",\"text\":\"%s\"}",escaped);
    return xz_ws_send_text(ws,body);
}
static int session_create(xz_ws *ws,int observer) {
    char voice[256],prompt[4096],body[6144];
    if (quote(env("DB_VOICE","zh_female_xiaohe_jupiter_bigtts"),voice,sizeof(voice)) ||
        quote(observer?"你是控制词监听助手，所有输入只回答好。":
              env("DB_INSTRUCTIONS","你是豆包，用简洁自然的中文连续对话。"),prompt,sizeof(prompt))) return -1;
    snprintf(body,sizeof(body),"{\"type\":\"session.create\",\"session\":{\"type\":\"realtime\","
        "\"model\":\"1.2.6.1\",\"audio\":{\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}},"
        "\"output\":{\"format\":{\"type\":\"%s\",\"rate\":24000},\"voice\":\"%s\",\"speed\":0,\"loudness\":0}},"
        "\"tools\":[],\"instructions\":\"%s\"},\"extension\":{\"asr\":%s,\"tts\":{},\"dialog\":{\"extra\":{\"enable_user_query_exit\":false}}}}",!strcmp(env("DB_AUDIO_FORMAT","ogg_opus"),"pcm_s16le")?"pcm_s16le":"ogg_opus",voice,prompt,
        observer?"{\"extra\":{\"enable_asr_twopass\":true}}":"{}");
    return xz_ws_send_text(ws,body);
}
/* Only ASR event handlers call this: output text can never end a session. */
static int transcript(const xz_json_doc *doc,int object,char *out,size_t cap) {
    if (doc->tokens[object].type==XZ_JSON_OBJECT) {
        if (!xz_json_get_string(doc,object,"transcript",out,cap) ||
            !xz_json_get_string(doc,object,"text",out,cap)) return 0;
    }
    for (int i=doc->tokens[object].first_child;i>=0;i=doc->tokens[i].next_sibling)
        if ((doc->tokens[i].type==XZ_JSON_OBJECT || doc->tokens[i].type==XZ_JSON_ARRAY) &&
            !transcript(doc,i,out,cap)) return 0;
    out[0]=0; return -1;
}
static int decode_audio(const char *text,unsigned char *out) {
    size_t n=strlen(text); if (!n || n%4) return -1;
    for (size_t i=0;i<n;i++) {
        char c=text[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='+'||c=='/'||
            (c=='=' && i>=n-2))) return -1;
    }
    int got=EVP_DecodeBlock(out,(const unsigned char *)text,(int)n);
    if (got<0) return -1;
    if (text[n-1]=='=') got--;
    if (text[n-2]=='=') got--;
    return got; /* WebSocket deltas may split a 16-bit PCM sample. */
}
typedef struct {
    xz_player player;
    db_echo *echo;
    db_ogg ogg;int ogg_mode,ogg_done;
    int active, sealed, pending_done, enabled;
    unsigned prebuffer_ms;
    long long echo_tail_until;
    unsigned char pending[262144]; size_t used;
} playback;
static void playback_stop(playback *p) {
    if (p->active) {
        xz_player_destroy(&p->player); p->active=0;
        p->echo_tail_until=now_ms()+180;
    }
    db_ogg_reset(&p->ogg);p->ogg_done=0;
    p->sealed=p->pending_done=0; p->used=0;
}
static int playback_start(playback *p) {
    if (!p->enabled || p->active) return 0;
    if (xz_player_init(&p->player)) return -1;
    p->player.echo=p->echo;
    p->active=1; p->sealed=0;
    return xz_player_start(&p->player,env("DB_APLAY","aplay"),env("DB_ALSA_DEVICE","notify"),
                           24000,p->prebuffer_ms);
}
static int playback_write(playback *p,const unsigned char *data,size_t len) {
    if (!p->enabled) return 0;
    if (p->sealed) {
        if (p->used+len>sizeof(p->pending)) return -1;
        memcpy(p->pending+p->used,data,len);p->used+=len;return 0;
    }
    if (playback_start(p)) return -1;
    return xz_player_write(&p->player,data,len);
}
static void playback_finish(playback *p) {
    if (!p->active) return;
    if (p->sealed) p->pending_done=1;
    else { xz_player_finish(&p->player);p->sealed=1; }
}
static int playback_tick(playback *p) {
    size_t prebuffer_bytes=(size_t)p->prebuffer_ms*48;
    size_t decode_limit=prebuffer_bytes>24000?prebuffer_bytes:24000;
    if(p->ogg_mode && p->enabled && (!p->active || xz_player_queued(&p->player)<decode_limit)) {
        int16_t pcm[2880];int samples=db_ogg_next(&p->ogg,pcm,2880);
        if(samples<0)return -1;
        if(samples && playback_write(p,(unsigned char *)pcm,(size_t)samples*2))return -1;
        if(p->ogg_done && db_ogg_empty(&p->ogg)) {
            printf("doubao: compressed playback drained peak_bytes=%zu tmp_bytes=%zu\n",p->ogg.peak,db_ogg_storage(&p->ogg));
            playback_finish(p);p->ogg_done=0;db_ogg_reset(&p->ogg);
        }
    }
    int code=0;
    if (!p->active || !xz_player_poll(&p->player,&code)) return 0;
    xz_player_destroy(&p->player);p->active=0;p->sealed=0;
    p->echo_tail_until=now_ms()+180;
    if (code) return -1;
    if (p->used) {
        if (playback_start(p) || xz_player_write(&p->player,p->pending,p->used)) return -1;
        p->used=0;
        if (p->pending_done) { playback_finish(p);p->pending_done=0; }
    }
    return 0;
}
static int protected_output(playback *p,int speaking,int waiting) {
    return speaking || waiting || (p->enabled && (p->active || p->ogg_done || !db_ogg_empty(&p->ogg)));
}
static int audible_output(const playback *p,int speaking) {
    return (p->active && xz_player_playback_started(&p->player)) || (!p->enabled && speaking);
}
static unsigned mean_pcm_level(const unsigned char *pcm,size_t bytes) {
    unsigned level=0;
    for(size_t i=0;i<bytes;i+=2) {
        int16_t value;
        memcpy(&value,pcm+i,sizeof(value));
        level+=abs(value);
    }
    return level/(bytes/2);
}
static int cancel_streaming_reply(xz_ws *ws,int streaming,long long *deadline) {
    if(!streaming)return 0;
    if(simple(ws,"response.cancel"))return -1;
    *deadline=now_ms()+500;
    return 1;
}
static void socket_options(xz_ws *ws) {
    struct timeval timeout={2,0};
    setsockopt(ws->fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(ws->fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    fcntl(ws->fd,F_SETFD,FD_CLOEXEC);
}
static int microphone_packet(xz_ws *ws,const unsigned char *pcm,size_t n) {
    char encoded[860],body[960];
    int bytes=EVP_EncodeBlock((unsigned char *)encoded,pcm,(int)n);encoded[bytes]=0;
    snprintf(body,sizeof(body),"{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",encoded);
    return xz_ws_send_text(ws,body);
}

/* Configuration verification opens one authenticated session, without audio,
 * ASR, player, lights or a conversation. Never infer validity from key shape. */
static int check_connection(const char *url,const char *key) {
    xz_ws ws;memset(&ws,0,sizeof(ws));ws.fd=-1;
    int rc=1;
    unsigned char *message=malloc(XZ_WS_MAX_MESSAGE+1);
    xz_json_doc *doc=malloc(sizeof(*doc));
    if(!message||!doc)goto done;
    if(xz_ws_connect(&ws,url,"","",key,8000))goto done;
    socket_options(&ws);
    if(session_create(&ws,1))goto done;
    long long until=now_ms()+12000;
    while(!stopping&&now_ms()<until) {
        int type=0;size_t length=0;
        int got=xz_ws_receive(&ws,&type,message,XZ_WS_MAX_MESSAGE,&length,100);
        if(got<0||type==XZ_WS_CLOSED)break;
        if(!got)continue;
        message[length]=0;char event[128];
        if(xz_json_parse(doc,(char*)message,length)||xz_json_get_string(doc,doc->root,"type",event,sizeof(event)))break;
        if(!strcmp(event,"session.created")||!strcmp(event,"session.updated")){rc=0;break;}
        if(!strcmp(event,"error"))break;
    }
done:
    if(ws.connected)(void)simple(&ws,"session.close");
    xz_ws_close(&ws);free(message);free(doc);
    puts(rc?"doubao: connection check failed":"doubao: connection check passed");
    return rc;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);setvbuf(stderr,NULL,_IOLBF,0);
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    const char *key=env("DB_API_KEY","");
    if (!*key || !strcmp(key,"填写你的APIKey")) { puts("doubao: 请先填写脚本顶部 DB_API_KEY");return 2; }
    const char *url=env("DB_ENDPOINT","wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue");
    /* Local fixture only; a real key must never be sent over plaintext. */
    if (strncmp(url,"wss://",6) && (strcmp(key,"fixture-key") || strncmp(url,"ws://127.0.0.1:",15))) return 2;
    if(argc==2&&!strcmp(argv[1],"--check"))return check_connection(url,key);
    if(argc!=1)return 2;
    const char *bye=env("DB_GOODBYE","豆包再见,再见豆包,拜拜了哦");
    const int barge_any=(int)number("DB_BARGE_ANY",1,0,1);
    const char *interrupt=getenv("DB_INTERRUPT");
    if (!interrupt || (!*interrupt && !barge_any))
        interrupt="等一下,等等,停一下,你说的不对";
    const int xiaomi_asr=!strcmp(env("DB_CONTROL_ASR","doubao"),"xiaomi");
    const int human_guard=barge_any && xiaomi_asr;
    const int native_barge=barge_any && !human_guard;
    const long idle=number("DB_IDLE_SECONDS",120,1,3600)*1000;
    const long maximum=number("DB_MAX_SECONDS",900,1,86400)*1000;
    int rc=1, mic=-1, ready=0, suppressed=0, speaking=0, mic_seen=0;
    int main_created=0,guard_ready=0,waiting=0,cancel_pending=0;
    int primary_muted=0,interrupt_consumed=0;
    int wait_barge_question=0;
    unsigned primary_quiet_ms=0,keyword_quiet_ms=0;
    long long reply_cancel_until=0;
    long long tail_partial_at=0,next_tail_check=0;
    uint64_t reply_serial=1;
    int main_asr_active=0,main_commit_pending=0;
    long long resume_final_at=0,last_primary_partial=0;
    int guarding=0,guard_consumed=0,guard_commit_pending=0;
    unsigned guard_epoch=0,utterance_epoch=0;
    long long last_guard_audio=0,guard_text_since=0;
    xz_ws ws;memset(&ws,0,sizeof(ws));ws.fd=-1;
    xz_ws guard;memset(&guard,0,sizeof(guard));guard.fd=-1;
    lx06_control_asr *local_guard=NULL;
    char spoken_text[4096]="";
    char tail_partial[2048]="";
    int spoken_text_open=0;
    const char *greeting=getenv("DB_GREETING");
    if(!greeting)greeting="我在呢，请说。";
    int greeting_pending=0;
    db_echo echo={
        .lock=PTHREAD_MUTEX_INITIALIZER,
        .speech_rms=barge_any?(unsigned)number("DB_BARGE_RMS",DB_BARGE_DEFAULT_RMS,DB_BARGE_MIN_RMS,DB_BARGE_MAX_RMS):0,
        .strong_protection=barge_any?(unsigned)number("DB_ECHO_PROTECTION",0,0,1):0,
    };
    unsigned echo_frames=0;
    playback *p=calloc(1,sizeof(*p));
    unsigned char *message=malloc(XZ_WS_MAX_MESSAGE+1), *audio=malloc(XZ_WS_MAX_MESSAGE);
    char *delta=malloc(XZ_WS_MAX_MESSAGE+1);
    xz_json_doc *doc=malloc(sizeof(*doc));
    if (!p||!message||!audio||!delta||!doc) goto cleanup;
    p->enabled=number("DB_PLAYBACK",1,0,1);
    p->prebuffer_ms=(unsigned)number("DB_PLAYBACK_BUFFER_MS",500,100,1000);
    if(p->enabled) p->echo=&echo;
    p->ogg_mode=strcmp(env("DB_AUDIO_FORMAT","ogg_opus"),"pcm_s16le")!=0;
    printf("doubao: DB_BARGE_ANY=%d (%s); client revision=separate-interrupt-1\n",
           barge_any,human_guard?"speaker Xiaomi ASR; confirmed speech interruption":
           native_barge?"native speech interruption":xiaomi_asr?"speaker Xiaomi ASR; one Doubao session":"isolated keyword interruption; two cloud sessions");
    if(barge_any)printf("doubao: speech threshold=%u echo protection=%u\n",echo.speech_rms,echo.strong_protection);
    puts("doubao: connecting");
    if (xz_ws_connect(&ws,url,"","",key,8000)) { puts("doubao: connection failed (TLS/network/auth)");goto cleanup; }
    socket_options(&ws);
    if(xiaomi_asr) {
        local_guard=lx06_control_asr_open(env("LX06_CONTROL_ASR_SOCKET",""));
        if(!local_guard){puts("doubao: speaker control ASR unavailable; protected mode requires bridge");goto cleanup;}
        guard_ready=1;
    } else if(!barge_any) {
        if(xz_ws_connect(&guard,url,"","",key,8000)) {puts("doubao: keyword observer connection failed; no unguarded fallback");goto cleanup;}
        socket_options(&guard);
    }
    if (session_create(&ws,0) || (!barge_any && !xiaomi_asr && session_create(&guard,1))) goto cleanup;
    long long started=now_ms(),last_activity=started,last_mic=started,last_event=started,last_ping=started;
    long long next_mic=started; unsigned long received_audio=0, sent_pcm=0;
    /* The service recommends fixed 20 ms packets. A nonblocking FIFO read
     * may return only one 10 ms tap frame; retain it until the pair is ready. */
    unsigned char mic_frame[640];size_t mic_used=0;
    unsigned char resume_pcm[160000];size_t resume_head=0,resume_used=0;
    const size_t resume_capacity=human_guard?sizeof(resume_pcm):64000;
    size_t resume_before_final=0;
    for (;!stopping;) {
        long long now=now_ms();
        if(cancel_pending && reply_cancel_until && now>=reply_cancel_until)
            cancel_pending=0;
        if(resume_final_at && !resume_before_final && !cancel_pending && !main_asr_active &&
           now-resume_final_at>=4000)
            resume_final_at=0;
        if(xiaomi_asr && !human_guard && resume_final_at && !resume_before_final && now-resume_final_at>=600 &&
           now-last_primary_partial>=600 && interrupt_consumed && main_asr_active &&
           !main_commit_pending && !cancel_pending && !protected_output(p,speaking,waiting)) {
            if(simple(&ws,"input_audio_buffer.commit"))break;
            main_commit_pending=1;resume_final_at=0;
            puts("doubao: completed listening utterance; requesting primary final");
        }
        /* The primary service can keep a partial open at full playback
         * volume even after the room has gone quiet. Give an observed
         * listening turn one endpoint request after its partials stop. */
        if(human_guard && main_asr_active && last_primary_partial && now-last_primary_partial>=1200 &&
           primary_quiet_ms>=640 &&
           !main_commit_pending && !cancel_pending && !protected_output(p,speaking,waiting)) {
            if(simple(&ws,"input_audio_buffer.commit"))break;
            main_commit_pending=1;
            puts("doubao: stable primary speech; requesting final");
        }
        if (now-started>=maximum) { puts("doubao: maximum session duration");rc=0;break; }
        if (!ready && now-started>12000) { puts("doubao: session creation timeout");break; }
        if (ready && !speaking && !p->active && now-last_activity>=idle) { puts("doubao: idle timeout");rc=0;break; }
        if (ready && now-last_mic>5000) { puts("doubao: microphone timeout");break; }
        if (speaking && now-last_event>30000) { puts("doubao: output stalled");break; }
        if (ready && now-last_ping>15000) {
            if (xz_ws_send_control(&ws,9,(unsigned char *)"db",2)) break;
            if (!barge_any && !xiaomi_asr && xz_ws_send_control(&guard,9,(unsigned char *)"db",2)) break;
            last_ping=now;
        }
        int was_playing=p->active;
        if (playback_tick(p)) { puts("doubao: player failed");break; }
        if(was_playing && !p->active && !speaking && !p->ogg_done && db_ogg_empty(&p->ogg))
            (void)lx06_dialog_light(LX06_LIGHT_LISTENING);
        if (p->active) last_activity=now;
        int event_type=0;size_t length=0;
        int result=xz_ws_receive(&ws,&event_type,message,XZ_WS_MAX_MESSAGE,&length,0);
        if (result<0) { puts("doubao: network closed/error");break; }
        if (result>0) {
            if (event_type==XZ_WS_CLOSED) { puts("doubao: remote websocket closed");break; }
            message[length]=0;
            if (xz_json_parse(doc,(char *)message,length)) { puts("doubao: malformed event");break; }
            char type[128];
            if (xz_json_get_string(doc,doc->root,"type",type,sizeof(type))) break;
            last_event=now;
            if (strcmp(type,"response.output_audio.delta") && strcmp(type,"response.output_text.delta"))
                printf("doubao: event=%s\n",type);
            if (!strcmp(type,"session.created") || !strcmp(type,"session.updated")) {
                main_created=1;
            } else if (!strcmp(type,"response.output_audio.started") && !wait_barge_question) {
                if(!waiting)reply_serial++;
                resume_final_at=0;resume_before_final=0;
                interrupt_consumed=0;
                tail_partial_at=0;
                keyword_quiet_ms=0;
                speaking=1;waiting=0;suppressed=0;received_audio=0;last_activity=now;p->ogg_done=0;
                /* Text and audio have independent start times. The first
                 * audio event must not erase the text already in this reply. */
                if(!spoken_text_open)spoken_text[0]=0;
                else if(*spoken_text)
                    printf("doubao: preserving playback text at audio start bytes=%zu\n",strlen(spoken_text));
                spoken_text_open=1;
                if(greeting_pending) {
                    if(!*spoken_text)snprintf(spoken_text,sizeof(spoken_text),"%s",greeting);
                    greeting_pending=0;
                }
                if(human_guard)resume_head=resume_used=0;
                if(!p->active) (void)lx06_dialog_light(LX06_LIGHT_THINKING);
            } else if (!strcmp(type,"response.output_audio.delta") && !suppressed) {
                if (xz_json_get_string(doc,doc->root,"delta",delta,XZ_WS_MAX_MESSAGE+1)) break;
                int bytes=decode_audio(delta,audio);
                if (bytes<0) { printf("doubao: invalid base64 audio chars=%zu\n",strlen(delta));break; }
                int queued=p->ogg_mode && p->enabled ? db_ogg_feed(&p->ogg,audio,(size_t)bytes) : playback_write(p,audio,(size_t)bytes);
                if (queued) { printf("doubao: player queue failure code=%d chunk=%d queued=%zu compressed=%zu errno=%d\n",
                    queued,bytes,p->active ? xz_player_queued(&p->player):0,p->ogg.used,errno);break; }
                received_audio+=(unsigned)bytes;last_activity=now;
            } else if (!strcmp(type,"response.output_text.delta") && !wait_barge_question) {
                char part[2048];
                if(!xz_json_get_string(doc,doc->root,"delta",part,sizeof(part))) {
                    if(!spoken_text_open)spoken_text[0]=0;
                    spoken_text_open=1;
                    size_t used=strlen(spoken_text),n=strlen(part);
                    if(used+n>=sizeof(spoken_text)) {
                        size_t drop=used+n-sizeof(spoken_text)+1;
                        memmove(spoken_text,spoken_text+drop,used-drop+1);used-=drop;
                    }
                    memcpy(spoken_text+used,part,n+1);
                }
            } else if (!strcmp(type,"response.output_audio.done")) {
                if (!suppressed) { if(p->ogg_mode)p->ogg_done=1;else playback_finish(p); }
                speaking=0;last_activity=now;
                printf("doubao: audio done bytes=%lu compressed_pending=%zu peak_bytes=%zu tmp_bytes=%zu\n",received_audio,p->ogg.used,p->ogg.peak,db_ogg_storage(&p->ogg));
            } else if (!strcmp(type,"response.done")) {
                /* A canceled/empty response may have started but contain no
                 * audio.done. Never leave the microphone protected forever. */
                if(speaking && !suppressed && received_audio) {
                    if(p->ogg_mode)p->ogg_done=1;else playback_finish(p);
                }
                speaking=waiting=0;last_activity=now;
                /* Retain the reference while the physical player drains. A
                 * later text delta or query opens the next reply's reference. */
                spoken_text_open=0;
                if(xiaomi_asr && cancel_pending) {
                    cancel_pending=0;puts("doubao: cancellation teardown complete");
                }
            } else if (!strcmp(type,"response.canceled")) {
                /* ACK only: local cancellation already stopped the old player.
                 * A late ACK must not stop audio from a subsequent response. */
                if(cancel_pending) {
                    if(!xiaomi_asr || human_guard)cancel_pending=0;
                    puts("doubao: cancellation acknowledged");
                }
            } else if (!strcmp(type,"conversation.item.input_audio_transcription.started") ||
                       !strcmp(type,"input_audio_buffer.speech_started")) {
                if(!main_asr_active){main_commit_pending=0;last_primary_partial=0;}
                main_asr_active=1;
                last_activity=now;
                if (native_barge && (speaking || p->active)) {
                    int cancel_remote=speaking;
                    long long stop_at=now_ms();
                    playback_stop(p);speaking=waiting=0;suppressed=1;interrupt_consumed=1;
                    long long stop_ms=now_ms()-stop_at;
                    (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                    cancel_pending=cancel_streaming_reply(&ws,cancel_remote,&reply_cancel_until);
                    if(cancel_pending<0)break;
                    printf("doubao: barge-in speech; player stopped, connection retained; player_stop_ms=%lld\n",stop_ms);
                }
            } else if (!strcmp(type,"conversation.item.input_audio_transcription.delta")) {
                last_primary_partial=now;
            } else if (!strcmp(type,"conversation.item.input_audio_transcription.completed")) {
                resume_final_at=0;resume_before_final=0;
                last_primary_partial=0;
                main_asr_active=main_commit_pending=0;
                char text[2048]; last_activity=now;
                if (!transcript(doc,doc->root,text,sizeof(text))) {
                    if(wait_barge_question && (!*text || xz_text_is_echo(text,spoken_text))) continue;
                    printf("doubao: ASR %s\n",text);
                    if(xiaomi_asr && !human_guard && (xz_text_matches_list(text,bye)||xz_text_matches_list_exact(text,interrupt))) {
                        puts("doubao: primary control transcript ignored; speaker ASR owns controls");
                        continue;
                    }
                    if ((!xiaomi_asr || human_guard) && xz_text_matches_list(text,bye)) {
                        playback_stop(p);puts("doubao: goodbye; closing session");rc=0;break;
                    }
                    if (!xiaomi_asr && (barge_any ? xz_text_matches_list(text,interrupt)
                                                  : xz_text_matches_list_exact(text,interrupt))) {
                        if(interrupt_consumed || cancel_pending || !protected_output(p,speaking,waiting)) {
                            puts("doubao: repeated/idle interrupt ignored; microphone retained");continue;
                        }
                        playback_stop(p);speaking=0;waiting=0;suppressed=1;
                        interrupt_consumed=1;
                        (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                        if (simple(&ws,"response.cancel")) break;
                        cancel_pending=1;
                        puts("doubao: interrupt phrase; connection retained");
                    } else {
                        wait_barge_question=0;
                        interrupt_consumed=0;
                        if(!native_barge){
                            waiting=1;reply_serial++;
                            spoken_text[0]=0;spoken_text_open=1;
                        }
                        if(!p->active) (void)lx06_dialog_light(LX06_LIGHT_THINKING);
                    }
                }
            } else if (!strcmp(type,"session.closed")) { rc=0;break; }
            else if (!strcmp(type,"error")) {
                /* Do not echo arbitrary server bodies containing credentials. */
                puts("doubao: server error; check service permission, quota and model");break;
            }
        }
        if(xiaomi_asr) {
            int replying=protected_output(p,speaking,waiting);
            int protect=replying||cancel_pending;
            int observe=!human_guard || (audible_output(p,speaking) && !cancel_pending);
            /* Keep control ASR across playback/listening boundaries. Canceling
             * it at audio.done loses a goodbye spoken over the last syllable. */
            if(lx06_control_asr_feed(local_guard,observe,NULL,0,(uint64_t)now))break;
            lx06_control_asr_reply(local_guard,replying?reply_serial:0);
            char text[2048];int got=lx06_control_asr_poll(local_guard,text,sizeof(text),(uint64_t)now);
            if(got<0){puts("doubao: speaker control ASR disconnected; closing protected session");break;}
            if(!barge_any && !got && replying && !cancel_pending && ready &&
               keyword_quiet_ms>=280 && now-last_mic<=100) {
                uint64_t partial_reply=0;
                const char *partial=lx06_control_asr_partial(local_guard,&partial_reply);
                if(partial_reply==reply_serial && xz_text_matches_list_exact(partial,interrupt) &&
                   !xz_self_echo(partial,spoken_text,"",bye,interrupt)) {
                    int ended=lx06_control_asr_end_utterance(local_guard,reply_serial,(uint64_t)now);
                    if(ended<0)break;
                    if(ended)puts("doubao: keyword candidate ended; requesting final");
                }
            }
            int tail_interrupt=0;
            if(human_guard && !got && now>=next_tail_check) {
                next_tail_check=now+25;
                uint64_t partial_reply=0;
                const char *partial=lx06_control_asr_partial(local_guard,&partial_reply);
                /* response.done gives us the complete playback transcript
                 * even when the speaker still holds several seconds of PCM. */
                if(observe && p->active && !speaking && !spoken_text_open &&
                   resume_used && *spoken_text && partial_reply==reply_serial &&
                   xz_text_has_speech(partial,4) && !xz_text_is_echo(partial,spoken_text)) {
                    if(!tail_partial_at || strcmp(partial,tail_partial)) {
                        snprintf(tail_partial,sizeof(tail_partial),"%s",partial);
                        tail_partial_at=now;
                    }
                    tail_interrupt=now-tail_partial_at>=75;
                } else tail_partial_at=0;
            }
            if(got>0 || tail_interrupt) {
                uint64_t recognized_reply=lx06_control_asr_result_reply(local_guard);
                if(human_guard) {
                    tail_partial_at=0;
                    if(!tail_interrupt && (!observe || !resume_used || recognized_reply!=reply_serial ||
                       !*spoken_text || !xz_text_has_speech(text,2) || xz_text_is_echo(text,spoken_text))) {
                        resume_head=resume_used=0;
                        puts("doubao: playback speech rejected by echo check");
                        continue;
                    }
                    if(got>0 && xz_text_matches_list(text,bye)) {
                        playback_stop(p);puts("doubao: goodbye; closing session");rc=0;break;
                    }
                    /* Replay the existing filtered lead-in too. The first
                     * syllables can be quieter than the frame that confirms
                     * interruption, so trimming back from detection loses
                     * the start of a valid question. */
                    int cancel_remote=speaking;
                    long long stop_at=now_ms();
                    playback_stop(p);
                    long long stop_ms=now_ms()-stop_at;
                    speaking=waiting=0;suppressed=1;interrupt_consumed=1;
                    wait_barge_question=1;
                    main_asr_active=main_commit_pending=0;
                    last_primary_partial=0;primary_quiet_ms=0;
                    (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                    cancel_pending=cancel_streaming_reply(&ws,cancel_remote,&reply_cancel_until);
                    if(cancel_pending<0)break;
                    last_activity=now;
                    printf("doubao: barge-in speech; independent speech confirmed, source=%s player_stop_ms=%lld\n",
                           tail_interrupt?"buffered-tail":"asr-final",stop_ms);
                    continue;
                }
                printf("doubao: control ASR %s\n",text);
                if(protect && *spoken_text && xz_self_echo(text,spoken_text,"",bye,interrupt)) {
                    puts("doubao: control self echo ignored");continue;
                }
                if(xz_text_matches_list(text,bye)) {
                    playback_stop(p);puts("doubao: goodbye; closing session");rc=0;break;
                }
                /* The control observer can finish the new question before
                 * teardown has released its queued audio to the main ASR.
                 * Retain an idle-attributed final. Commit only after the main
                 * ASR starts and its replayed PCM has reached the server. */
                if(interrupt_consumed && !protected_output(p,speaking,waiting) &&
                   !lx06_control_asr_result_reply(local_guard) && !main_commit_pending) {
                    resume_final_at=now;
                    resume_before_final=resume_used;
                }
                if(xz_text_matches_list_exact(text,interrupt)) {
                    if(interrupt_consumed || cancel_pending || !protected_output(p,speaking,waiting) ||
                       lx06_control_asr_result_reply(local_guard)!=reply_serial) {
                        puts("doubao: repeated/idle interrupt ignored; microphone retained");continue;
                    }
                    int cancel_remote=speaking||waiting;
                    playback_stop(p);speaking=waiting=0;suppressed=1;guard_consumed=1;guarding=0;
                    interrupt_consumed=1;
                    (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                    cancel_pending=cancel_streaming_reply(&ws,cancel_remote,&reply_cancel_until);
                    if(cancel_pending<0)break;
                    last_activity=now;
                    puts("doubao: interrupt phrase; connection retained (keyword observer)");
                } else puts("doubao: non-control speech ignored; reply retained");
            }
        } else if(!barge_any) {
            /* Share the parser/buffers with the main connection. Observer audio
             * is never decoded, queued or played; only final ASR controls act. */
            int kind=0;size_t n=0;
            int got=xz_ws_receive(&guard,&kind,message,XZ_WS_MAX_MESSAGE,&n,0);
            if(got<0 || (got>0 && kind==XZ_WS_CLOSED)) {puts("doubao: keyword observer disconnected; closing protected session");break;}
            if(got>0) {
                message[n]=0;char type[128];
                if(xz_json_parse(doc,(char *)message,n) ||
                   xz_json_get_string(doc,doc->root,"type",type,sizeof(type))) {puts("doubao: invalid observer event");break;}
                if(number("DB_TRACE_ASR",0,0,1) && strcmp(type,"response.output_audio.delta") &&
                   strcmp(type,"response.output_text.delta")) printf("doubao: observer event=%s epoch=%u/%u\n",type,utterance_epoch,guard_epoch);
                if(!strcmp(type,"session.created")) {guard_ready=1;puts("doubao: keyword observer ready");}
                else if(!strcmp(type,"error") || !strcmp(type,"session.closed")) {puts("doubao: keyword observer unavailable; closing protected session");break;}
                /* Discard the observer's brief answer without response.cancel:
                 * canceling here can also truncate the next control utterance
                 * when users speak again immediately after a non-control word. */
                else if(!strcmp(type,"conversation.item.input_audio_transcription.started")) {
                    utterance_epoch=last_guard_audio && now-last_guard_audio<2000?guard_epoch:0;
                    guard_commit_pending=0;guard_text_since=now;
                    puts("doubao: control speech started");
                } else if(!strcmp(type,"conversation.item.input_audio_transcription.delta")) {
                    /* This service sends the current ASR hypothesis in delta,
                     * not append-only characters. Force endpointing only after
                     * a full control phrase appears; cancellation still requires
                     * the final transcript, so partial corrections cannot stop TTS. */
                    char text[2048];
                    if(!xz_json_get_string(doc,doc->root,"delta",text,sizeof(text)) && *text) {
                        if(number("DB_TRACE_ASR",0,0,1))printf("doubao: control hypothesis %s\n",text);
                        if(!guard_text_since)guard_text_since=now;
                            if(!guard_commit_pending && utterance_epoch && utterance_epoch==guard_epoch &&
                           !guard_consumed && (xz_text_matches_list_exact(text,interrupt)||xz_text_matches_list(text,bye))) {
                            if(simple(&guard,"input_audio_buffer.commit"))break;
                            guard_commit_pending=1;
                            puts("doubao: control candidate; requesting final ASR");
                        }
                    }
                } else if(!strcmp(type,"conversation.item.input_audio_transcription.completed")) {
                    char text[2048];
                    if(!transcript(doc,doc->root,text,sizeof(text)) && utterance_epoch &&
                       utterance_epoch==guard_epoch && !guard_consumed) {
                        printf("doubao: control ASR %s\n",text);
                        if(xz_text_matches_list(text,bye)) {
                            playback_stop(p);puts("doubao: goodbye; closing session");rc=0;break;
                        }
                        if(xz_text_matches_list_exact(text,interrupt) &&
                           !xz_self_echo(text,spoken_text,"",bye,interrupt)) {
                            if(interrupt_consumed || cancel_pending || !protected_output(p,speaking,waiting)) {
                                utterance_epoch=0;guard_text_since=0;
                                puts("doubao: repeated/idle interrupt ignored; microphone retained");continue;
                            }
                            int cancel_remote=speaking||waiting;
                            playback_stop(p);speaking=waiting=0;suppressed=1;guard_consumed=1;guarding=0;
                            interrupt_consumed=1;
                            (void)lx06_dialog_light(LX06_LIGHT_INTERRUPTED);
                            cancel_pending=cancel_streaming_reply(&ws,cancel_remote,&reply_cancel_until);
                            if(cancel_pending<0)break;
                            last_activity=now;
                            puts("doubao: interrupt phrase; connection retained (keyword observer)");
                        } else puts("doubao: non-control speech ignored; reply retained");
                    }
                    utterance_epoch=0;guard_text_since=0;
                }
            }
            /* Continuous TV speech or residual echo must not keep the control
             * recognizer's utterance open indefinitely. A started event can
             * precede text for short commands; endpoint it too, once per turn. */
            if(guard_text_since && !guard_commit_pending && now-guard_text_since>=3000) {
                if(simple(&guard,"input_audio_buffer.commit"))break;
                guard_commit_pending=1;
            }
        }
        if(!ready && main_created && (barge_any || guard_ready)) {
            ready=1;last_activity=last_mic=now;
            mic=open(env("DB_MIC_FIFO","/dev/stdin"),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(mic<0){puts("doubao: microphone open failed");break;}
            puts("doubao: session ready");
            (void)lx06_dialog_light(LX06_LIGHT_LISTENING);
            if(*greeting) {
                waiting=!native_barge;
                greeting_pending=1;
                if(speech(&ws,greeting))break;
            }
        }
        if (ready && now>=next_mic) {
            ssize_t n=read(mic,mic_frame+mic_used,sizeof(mic_frame)-mic_used);
            if (n>0) {
                mic_used+=(size_t)n;last_mic=now;mic_seen=1;
                if(mic_used<sizeof(mic_frame))continue;
                static const unsigned char silence[640]={0};
                /* Keep speech captured during cancellation teardown, then
                 * send at the original frame cadence. Never burst a backlog
                 * or discard the beginning of an immediate follow-up. */
                int resuming=xiaomi_asr && cancel_pending;
                int replying=protected_output(p,speaking,waiting);
                int protect=!native_barge && (replying || resuming ||
                            (human_guard && !interrupt_consumed && now<p->echo_tail_until));
                if(xiaomi_asr && !human_guard && protect && !resuming)resume_head=resume_used=0;
                unsigned char filtered[sizeof(mic_frame)];
                const unsigned char *uplink=protect?silence:mic_frame;
                const unsigned char *candidate=mic_frame;
                if(p->echo) {
                    long long capture_at=now_ms();
                    int playback_audible=audible_output(p,speaking) || capture_at<p->echo_tail_until;
                    if(barge_any) {
                        if(db_echo_filter(p->echo,mic_frame,playback_audible,(uint64_t)capture_at,filtered) &&
                           !echo_frames++)
                            puts("doubao: echo guard protecting playback");
                        candidate=filtered;
                    } else if(playback_audible &&
                              db_echo_keyword_filter(p->echo,mic_frame,(uint64_t)capture_at,filtered)) {
                        candidate=filtered;
                    }
                    if(!protect)uplink=candidate;
                }
                if(!barge_any) {
                    if(!replying || mean_pcm_level(candidate,mic_used)>=180)keyword_quiet_ms=0;
                    else if(keyword_quiet_ms<1000)keyword_quiet_ms+=20;
                }
                if(human_guard && !replying && !interrupt_consumed)resume_head=resume_used=0;
                int audible_barge=human_guard && audible_output(p,speaking) && !resuming;
                int capture_barge=audible_barge &&
                    (resume_used || memcmp(candidate,silence,sizeof(silence)));
                if(xiaomi_asr && (resuming || capture_barge ||
                   (resume_used && (!human_guard || interrupt_consumed)))) {
                    /* Leave two seconds for frames arriving during cancel. */
                    if(capture_barge && resume_used>=96000) {
                        resume_head=(resume_head+mic_used)%resume_capacity;
                        resume_used-=mic_used;
                    }
                    if(resume_used+mic_used>resume_capacity) {
                        puts("doubao: cancellation audio backlog exceeded its limit");break;
                    }
                    size_t tail=(resume_head+resume_used)%resume_capacity;
                    memcpy(resume_pcm+tail,candidate,mic_used);resume_used+=mic_used;
                    if(!protect)uplink=resume_pcm+resume_head;
                }
                if(xiaomi_asr && protect!=primary_muted) {
                    /* Continuous silence keeps the input stream clocked.
                     * mute/unmute is for stopping upload entirely; mixing it
                     * with continuing frames races cancellation on this API. */
                    primary_muted=protect;
                    puts(protect?"doubao: primary microphone muted during protected playback":"doubao: primary microphone resumed");
                }
                if(protect && !guarding) {guard_epoch++;guard_consumed=0;}
                guarding=protect;
                if(protect)last_guard_audio=now;
                if(human_guard) {
                    if(protect || mean_pcm_level(uplink,mic_used)>=180) primary_quiet_ms=0;
                    else if(primary_quiet_ms<1000) primary_quiet_ms+=20;
                }
                if(microphone_packet(&ws,uplink,mic_used))break;
                if(xiaomi_asr && interrupt_consumed && number("DB_TRACE_ASR",0,0,1) && sent_pcm%32000==0) {
                    int energy=0;for(size_t i=0;i<mic_used;i+=2){int16_t value;memcpy(&value,uplink+i,2);energy+=abs(value);}
                    printf("doubao: resumed PCM protected=%d mean_abs=%d queued=%zu\n",protect,energy/320,resume_used);
                }
                if(xiaomi_asr && !protect && resume_used) {
                    resume_head=(resume_head+mic_used)%resume_capacity;resume_used-=mic_used;
                    if(resume_before_final) {
                        resume_before_final-=mic_used;
                        if(!resume_before_final)resume_final_at=now;
                    }
                }
                if(xiaomi_asr) {
                    if(lx06_control_asr_feed(local_guard,!human_guard||audible_barge,
                                           candidate,mic_used,(uint64_t)now))break;
                } else if(!barge_any && microphone_packet(&guard,protect?candidate:silence,mic_used))break;
                sent_pcm+=(unsigned)mic_used;mic_used=0;
                /* Real FIFO is already clocked by the device. Re-pacing each
                 * chunk accumulates loop jitter and delays spoken interrupts. */
                next_mic=number("DB_PACE_INPUT",0,0,1) ? now+20 : 0;
            } else if (!n && mic_seen) { puts("doubao: microphone closed");break; }
            else if (n<0 && errno!=EAGAIN && errno!=EINTR) break;
        }
        if (!result) poll(NULL,0,5);
    }
    if (stopping) { printf("doubao: stopped signal=%d\n",(int)stopping);rc=0; }
    printf("doubao: session finished input_bytes=%lu echo_frames=%u\n",sent_pcm,echo_frames);
cleanup:
    if (p) playback_stop(p);
    pthread_mutex_destroy(&echo.lock);
    (void)lx06_dialog_light(LX06_LIGHT_END);
    if (ws.connected && !stopping) (void)simple(&ws,"session.close");
    xz_ws_close(&ws);
    if (guard.connected && !stopping) (void)simple(&guard,"session.close");
    xz_ws_close(&guard);
    lx06_control_asr_close(local_guard);
    if (mic>=0) close(mic);
    free(p);free(message);free(audio);free(delta);free(doc);
    return rc;
}
