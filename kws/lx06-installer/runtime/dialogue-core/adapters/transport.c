#define _POSIX_C_SOURCE 200809L
#include "transport.h"
#include "support/xiaozhi/ws.h"
#include "support/doubao/ws.h"
#include "support/xiaozhi/protocol.h"
#include "support/xiaozhi/event.h"
#include "support/xiaozhi/opus_codec.h"
#include "support/common/json.h"
#include "support/common/ogg_audio.h"
#include <errno.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define DC_WIRE_CAP (512u*1024u)
struct dc_transport {
 struct dc_transport_config config;
 dc_transport_sink sink;void *opaque;
 dc_xz_ws xz;dc_db_ws db;
 xz_opus_codec codec;db_ogg ogg;
 char session_id[96],response_id[128],cancel_id[128];
 char retired_ids[8][128];unsigned retired_next;
 unsigned output_rate,ready,reply_open,ogg_done,discarding;
 uint64_t sequence,cancel_sequence,last_cancel_sequence;
 uint64_t ping_token,ping_pending,ping_sent_ms,ping_next_ms;
 unsigned char pcm[1920];size_t pcm_used;
 unsigned char *message,*decoded;char *text;
 xz_json_doc *doc;
};
static uint64_t milliseconds(void){struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;return (uint64_t)now.tv_sec*1000u+(unsigned long)now.tv_nsec/1000000u;}
static int heartbeat(dc_transport *t){
 if(t->config.provider!=DC_TRANSPORT_XIAOZHI||!t->ready||!t->config.ping_interval_ms)return 0;
 uint64_t now=milliseconds();if(!now)return -1;
 if(t->ping_pending&&t->xz.pong_token==t->ping_pending){t->ping_pending=0;t->ping_next_ms=now+t->config.ping_interval_ms;}
 if(t->ping_pending)return now-t->ping_sent_ms>=t->config.pong_timeout_ms?-1:0;
 if(now>=t->ping_next_ms)return dc_transport_ping(t);
 return 0;
}
static const char *nz(const char *s,const char *fallback){return s&&*s?s:fallback;}
static int retired(const dc_transport *t,const char *id){
 if(!id[0])return 0;
 for(unsigned i=0;i<8;i++)if(!strcmp(id,t->retired_ids[i]))return 1;
 return 0;
}
static void retire(dc_transport *t,const char *id){
 if(id[0]&&!retired(t,id))memcpy(t->retired_ids[t->retired_next++%8],id,strlen(id)+1);
}
static int event(dc_transport *t,enum dc_transport_event_type type,uint64_t sequence,const void *data,size_t n,unsigned rate,unsigned code){
 struct dc_transport_event out={type,sequence,rate,code,data,n};
 return t->sink(t->opaque,&out);
}
static int send_text(dc_transport *t,const char *s){
 return t->config.provider==DC_TRANSPORT_XIAOZHI?dc_xz_ws_send_text(&t->xz,s):dc_db_ws_send_text(&t->db,s);
}
static int simple(dc_transport *t,const char *type){
 char text[128];int n=snprintf(text,sizeof(text),"{\"type\":\"%s\"}",type);
 return n>0&&(size_t)n<sizeof(text)?send_text(t,text):-1;
}
static int quote(const char *in,char *out,size_t cap){
 size_t used=0;
 for(const unsigned char *s=(const unsigned char *)nz(in,"");*s;s++){
  size_t need=*s<32?6:(*s=='"'||*s=='\\'?2:1);
  if(used+need>=cap)return -1;
  if(*s<32){snprintf(out+used,cap-used,"\\u%04x",*s);used+=6;}
  else {if(need==2)out[used++]='\\';out[used++]=(char)*s;}
 }
 out[used]=0;return 0;
}
static int db_create(dc_transport *t){
 char voice[256],prompt[4096],body[6144];
 if(quote(nz(t->config.voice,"zh_female_xiaohe_jupiter_bigtts"),voice,sizeof(voice))||
    quote(nz(t->config.instructions,"你是豆包，用简洁自然的中文连续对话。"),prompt,sizeof(prompt)))return -1;
 int n=snprintf(body,sizeof(body),"{\"type\":\"session.create\",\"session\":{\"type\":\"realtime\",\"model\":\"1.2.6.1\",\"audio\":{\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}},\"output\":{\"format\":{\"type\":\"%s\",\"rate\":24000},\"voice\":\"%s\",\"speed\":0,\"loudness\":0}},\"tools\":[],\"instructions\":\"%s\"},\"extension\":{\"asr\":{},\"tts\":{},\"dialog\":{\"extra\":{\"enable_user_query_exit\":false}}}}",t->config.output_pcm?"pcm_s16le":"ogg_opus",voice,prompt);
 return n>0&&(size_t)n<sizeof(body)?send_text(t,body):-1;
}
static int begin(dc_transport *t){
 if(t->reply_open)return 0;
 if(t->sequence==UINT64_MAX)return -1;
 t->sequence++;t->reply_open=1;t->ogg_done=0;
 return event(t,DC_T_REPLY_BEGIN,t->sequence,NULL,0,t->output_rate,0);
}
static int end(dc_transport *t){
 if(!t->reply_open)return 0;
 t->reply_open=0;
 return event(t,DC_T_REPLY_END,t->sequence,NULL,0,t->output_rate,0);
}
static int db_pcm(dc_transport *t){
 if(t->config.output_pcm)return 0;
 for(unsigned work=0;work<32;work++){
  int16_t pcm[2880];int samples=db_ogg_next(&t->ogg,pcm,2880);
  if(samples<0)return -1;
  if(!samples)break;
  if(event(t,DC_T_PCM,t->sequence,pcm,(size_t)samples*2,24000,0))return -1;
 }
 if(t->ogg_done&&db_ogg_empty(&t->ogg)){t->ogg_done=0;db_ogg_reset(&t->ogg);return end(t);}
 return 0;
}
static int xz_message(dc_transport *t,int kind,const unsigned char *data,size_t n){
 if(kind==DC_XZ_WS_BINARY){
  if(t->discarding)return 0;
  int16_t pcm[2880];if(!t->ready)return -1;
  int samples=xz_opus_decode(&t->codec,data,n,pcm,2880);
  if(samples<0||begin(t))return -1;
  return event(t,DC_T_PCM,t->sequence,pcm,(size_t)samples*2,t->output_rate,0);
 }
 if(kind==DC_XZ_WS_CLOSED)return event(t,DC_T_REMOTE_CLOSE,t->sequence,NULL,0,0,0)?-1:1;
 if(kind!=DC_XZ_WS_TEXT)return -1;
 if(!t->ready){
  xz_hello hello;
  if(xz_protocol_parse_server_hello((const char *)data,n,&hello,t->session_id,sizeof(t->session_id))||
     hello.version!=1||hello.channels!=1||hello.frame_duration_ms!=60||
     (hello.sample_rate!=16000&&hello.sample_rate!=24000))return -1;
  if(xz_opus_codec_init(&t->codec,16000,hello.sample_rate))return -1;
  t->output_rate=(unsigned)hello.sample_rate;t->ready=1;t->ping_next_ms=milliseconds()+t->config.ping_interval_ms;
  return event(t,DC_T_READY,0,NULL,0,t->output_rate,0);
 }
 xz_server_event e;if(xz_event_parse_text((const char *)data,n,&e))return -1;
 switch(e.type){
 case XZ_EVENT_TTS:
  if(!strcmp(e.state,"start")){t->discarding=0;return begin(t);}
  if(!strcmp(e.state,"sentence_start")&&e.text_bytes){if(begin(t))return -1;return event(t,DC_T_ASSISTANT_TEXT,t->sequence,e.text,e.text_bytes,0,0);}
  if(!strcmp(e.state,"stop"))return t->discarding?0:end(t);
  return 0;
 case XZ_EVENT_STT:return event(t,DC_T_USER_FINAL,t->sequence,e.text,e.text_bytes,0,0);
 /* Xiaozhi abort events have no request identity. Never relabel a late abort
  * as an acknowledgement of a newer cancellation. Successful listen.start
  * below supplies the request-local RESUMED event instead. */
 case XZ_EVENT_ABORT:return 0;
 case XZ_EVENT_GOODBYE:return event(t,DC_T_REMOTE_CLOSE,t->sequence,NULL,0,0,0)?-1:1;
 case XZ_EVENT_ERROR:return event(t,DC_T_ERROR,t->sequence,NULL,0,0,1)?-1:1;
 default:return 0;
 }
}
static int transcript(const xz_json_doc *doc,int object,char *out,size_t cap){
 if(doc->tokens[object].type==XZ_JSON_OBJECT&&
    (!xz_json_get_string(doc,object,"transcript",out,cap)||!xz_json_get_string(doc,object,"text",out,cap)))return 0;
 for(int i=doc->tokens[object].first_child;i>=0;i=doc->tokens[i].next_sibling)
  if((doc->tokens[i].type==XZ_JSON_OBJECT||doc->tokens[i].type==XZ_JSON_ARRAY)&&!transcript(doc,i,out,cap))return 0;
 out[0]=0;return -1;
}
static int audio_decode(dc_transport *t,size_t *bytes){
 if(xz_json_get_string(t->doc,t->doc->root,"delta",t->text,DC_WIRE_CAP+1))return -1;
 size_t n=strlen(t->text);if(!n||n%4||n>DC_WIRE_CAP)return -1;
 for(size_t i=0;i<n;i++){
  char c=t->text[i];
  if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'||(c=='='&&i>=n-2)))return -1;
 }
 int decoded=EVP_DecodeBlock(t->decoded,(unsigned char *)t->text,(int)n);if(decoded<0)return -1;
 if(t->text[n-1]=='=')decoded--;
 if(n>1&&t->text[n-2]=='=')decoded--;
 *bytes=(size_t)decoded;return 0;
}
static int db_message(dc_transport *t,int kind,const unsigned char *data,size_t n){
 if(kind==DC_DB_WS_CLOSED)return event(t,DC_T_REMOTE_CLOSE,t->sequence,NULL,0,0,0)?-1:1;
 if(kind!=DC_DB_WS_TEXT||xz_json_parse(t->doc,(const char *)data,n))return -1;
 char type[128];if(xz_json_get_string(t->doc,t->doc->root,"type",type,sizeof(type)))return -1;
 char response[128]="";
 if(xz_json_get_string(t->doc,t->doc->root,"response_id",response,sizeof(response))){
  int object=xz_json_object_get(t->doc,t->doc->root,"response");
  if(object>=0)(void)xz_json_get_string(t->doc,object,"id",response,sizeof(response));
 }
 int canceled_id=response[0]&&t->cancel_id[0]&&!strcmp(response,t->cancel_id);
 int retired_id=retired(t,response);
 if(!strcmp(type,"session.created")||!strcmp(type,"session.updated")){
  if(!t->ready){t->ready=1;t->output_rate=24000;return event(t,DC_T_READY,0,NULL,0,24000,0);}return 0;
 }
 if(!strcmp(type,"response.output_audio.started")){
  if(retired_id||t->cancel_sequence)return 0;
  if(response[0])memcpy(t->response_id,response,strlen(response)+1);
  t->discarding=0;return begin(t);
 }
 if(!strcmp(type,"response.output_audio.delta")){
  if(t->discarding||retired_id)return 0;
  size_t size;if(!t->ready||audio_decode(t,&size)||begin(t))return -1;
  if(t->config.output_pcm)return size%2?-1:event(t,DC_T_PCM,t->sequence,t->decoded,size,24000,0);
  if(db_ogg_feed(&t->ogg,t->decoded,size))return -1;
  return db_pcm(t);
 }
 if(!strcmp(type,"response.output_text.delta")){
  if(t->discarding||retired_id)return 0;
  if(xz_json_get_string(t->doc,t->doc->root,"delta",t->text,DC_WIRE_CAP+1)||begin(t))return -1;
  return event(t,DC_T_ASSISTANT_TEXT,t->sequence,t->text,strlen(t->text),0,0);
 }
 if(!strcmp(type,"response.output_audio.done")){
  if(t->discarding||retired_id)return 0;
  if(t->config.output_pcm)return end(t);
  t->ogg_done=1;return db_pcm(t);
 }
 if(!strcmp(type,"response.done")){
  /* Protocol teardown, including empty/cancelled output. Core decides when
   * to resume the protected microphone; a late cancel ACK never stops audio. */
  if(t->cancel_sequence&&(!response[0]||canceled_id)){uint64_t seq=t->cancel_sequence;t->cancel_sequence=0;t->discarding=0;return event(t,DC_T_LISTEN_READY,seq,NULL,0,0,0);}
  if(retired_id)return 0;
  if(t->config.output_pcm){if(end(t))return -1;}
  else {t->ogg_done=1;if(db_pcm(t))return -1;}
  return 0;
 }
 if(!strcmp(type,"response.canceled"))return canceled_id&&t->cancel_sequence?event(t,DC_T_CANCEL_ACK,t->cancel_sequence,NULL,0,0,0):0;
 if(!strcmp(type,"conversation.item.input_audio_transcription.started")||!strcmp(type,"input_audio_buffer.speech_started"))return event(t,DC_T_SPEECH_STARTED,t->sequence,NULL,0,0,0);
 if(!strcmp(type,"conversation.item.input_audio_transcription.delta")||!strcmp(type,"conversation.item.input_audio_transcription.completed")){
  char text[2048];if(transcript(t->doc,t->doc->root,text,sizeof(text)))return 0;
  return event(t,strstr(type,"completed")?DC_T_USER_FINAL:DC_T_USER_PARTIAL,t->sequence,text,strlen(text),0,0);
 }
 if(!strcmp(type,"session.closed"))return event(t,DC_T_REMOTE_CLOSE,t->sequence,NULL,0,0,0)?-1:1;
 if(!strcmp(type,"error"))return event(t,DC_T_ERROR,t->sequence,NULL,0,0,1)?-1:1;
 return 0;
}
dc_transport *dc_transport_open(const struct dc_transport_config *config,dc_transport_sink sink,void *opaque){
 if(!config||!sink||!config->endpoint||
    (config->provider!=DC_TRANSPORT_XIAOZHI&&config->provider!=DC_TRANSPORT_DOUBAO))return NULL;
 /* Plaintext transport is reserved for credential-free/synthetic loopback
  * fixtures. A personal credential can never be sent to an insecure URL. */
 if(strncmp(config->endpoint,"wss://",6)&&
    (strncmp(config->endpoint,"ws://127.0.0.1:",15)||strcmp(nz(config->credential,""),"fixture-key")))return NULL;
 dc_transport *t=calloc(1,sizeof(*t));if(!t)return NULL;
 t->config=*config;t->sink=sink;t->opaque=opaque;t->xz.fd=t->db.fd=-1;
 t->message=malloc(DC_WIRE_CAP+1);t->decoded=malloc(DC_WIRE_CAP);t->text=malloc(DC_WIRE_CAP+1);t->doc=malloc(sizeof(*t->doc));
 if(!t->message||!t->decoded||!t->text||!t->doc)goto fail;
 unsigned timeout=config->connect_timeout_ms?config->connect_timeout_ms:8000;
 if(timeout<100||timeout>60000)goto fail;
 if(config->provider==DC_TRANSPORT_XIAOZHI&&config->ping_interval_ms&&
    (config->ping_interval_ms<100||config->ping_interval_ms>300000||config->pong_timeout_ms<100||config->pong_timeout_ms>60000))goto fail;
 if(config->provider==DC_TRANSPORT_XIAOZHI){
  if(dc_xz_ws_connect(&t->xz,config->endpoint,nz(config->device_id,""),nz(config->client_id,""),nz(config->credential,""),(int)timeout))goto fail;
  char hello[512];if(xz_protocol_build_hello(hello,sizeof(hello),config->native_realtime)||send_text(t,hello))goto fail;
 }else {
  if(!config->credential||!*config->credential||dc_db_ws_connect(&t->db,config->endpoint,"","",config->credential,(int)timeout)||db_create(t))goto fail;
 }
 return t;
fail:dc_transport_close(t);return NULL;
}
int dc_transport_pcm(dc_transport *t,const void *data,size_t n){
 if(!t||!t->ready||(!data&&n)||n%2||n>32000)return -1;
 const unsigned char *p=data;size_t frame=t->config.provider==DC_TRANSPORT_XIAOZHI?1920:640;
 while(n){
  size_t take=frame-t->pcm_used;if(take>n)take=n;
  memcpy(t->pcm+t->pcm_used,p,take);t->pcm_used+=take;p+=take;n-=take;
  if(t->pcm_used==frame){
   if(t->config.provider==DC_TRANSPORT_XIAOZHI){
    unsigned char wire[4096];int size=xz_opus_encode(&t->codec,(int16_t *)t->pcm,960,wire,sizeof(wire));
    if(size<=0||dc_xz_ws_send_binary(&t->xz,wire,(size_t)size))return -1;
   }else {
    char encoded[860],body[960];int size=EVP_EncodeBlock((unsigned char *)encoded,t->pcm,640);encoded[size]=0;
    int written=snprintf(body,sizeof(body),"{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",encoded);
    if(written<0||(size_t)written>=sizeof(body)||send_text(t,body))return -1;
   }
   t->pcm_used=0;
  }
 }
 return 0;
}
int dc_transport_poll(dc_transport *t,unsigned timeout){
 if(!t||timeout>1000||heartbeat(t))return -1;
 if(t->config.provider==DC_TRANSPORT_DOUBAO&&db_pcm(t))return -1;
 int kind;size_t n=0;
 int got=t->config.provider==DC_TRANSPORT_XIAOZHI?dc_xz_ws_receive(&t->xz,&kind,t->message,DC_WIRE_CAP,&n,(int)timeout):dc_db_ws_receive(&t->db,&kind,t->message,DC_WIRE_CAP,&n,(int)timeout);
 if(got<=0)return got<0?got:heartbeat(t);
 t->message[n]=0;
 int result=t->config.provider==DC_TRANSPORT_XIAOZHI?xz_message(t,kind,t->message,n):db_message(t,kind,t->message,n);
 return result?result:heartbeat(t);
}
int dc_transport_listen(dc_transport *t){
 if(!t||!t->ready)return -1;
 if(t->config.provider==DC_TRANSPORT_DOUBAO)return 0;
 /* In guarded mode only the core's genuine QUESTION FINAL ends this input
  * turn. Server auto VAD could otherwise submit an incomplete first question. */
 char body[256];return xz_protocol_build_listen(body,sizeof(body),t->session_id,"start",t->config.native_realtime?"realtime":"manual")?-1:send_text(t,body);
}
int dc_transport_cancel(dc_transport *t,uint64_t seq){
 if(!t||!t->ready||!seq||seq>t->sequence)return -1;
 if(seq<=t->last_cancel_sequence)return 0;
 if(seq<t->sequence)return event(t,DC_T_CANCEL_ACK,seq,NULL,0,0,0);
 if(t->cancel_sequence==seq)return 0;
 if(t->config.provider==DC_TRANSPORT_DOUBAO&&t->cancel_sequence)return -1;
 t->cancel_sequence=seq;
 t->last_cancel_sequence=seq;
 memcpy(t->cancel_id,t->response_id,sizeof(t->cancel_id));
 retire(t,t->cancel_id);
 t->reply_open=0;t->discarding=1;
 if(t->config.provider==DC_TRANSPORT_DOUBAO){db_ogg_reset(&t->ogg);t->ogg_done=0;return simple(t,"response.cancel");}
 char body[256];
 if(xz_protocol_build_abort(body,sizeof(body),t->session_id,"wake_word_detected")||send_text(t,body))return -1;
 if(dc_transport_listen(t))return -1;
 return event(t,DC_T_LISTEN_READY,seq,NULL,0,0,0);
}
int dc_transport_commit(dc_transport *t){
 if(!t||!t->ready)return -1;
 /* Do not carry the user's final sub-frame into the next turn. Padding only
  * the transport boundary preserves the final 0-59ms/0-19ms of real speech. */
 if(t->pcm_used){
  unsigned char silence[1920]={0};
  size_t frame=t->config.provider==DC_TRANSPORT_XIAOZHI?1920:640;
  if(dc_transport_pcm(t,silence,frame-t->pcm_used))return -1;
 }
 if(t->config.provider==DC_TRANSPORT_DOUBAO)return simple(t,"input_audio_buffer.commit");
 char body[256];return xz_protocol_build_listen(body,sizeof(body),t->session_id,"stop",NULL)?-1:send_text(t,body);
}
int dc_transport_text(dc_transport *t,const char *text){
 if(!t||!t->ready||!text||!*text)return -1;
 char body[2304];
 if(t->config.provider==DC_TRANSPORT_XIAOZHI)return xz_protocol_build_detect(body,sizeof(body),t->session_id,text)?-1:send_text(t,body);
 char escaped[2048];if(quote(text,escaped,sizeof(escaped)))return -1;
 int n=snprintf(body,sizeof(body),"{\"type\":\"speech_text_buffer.commit\",\"text\":\"%s\"}",escaped);
 return n>0&&(size_t)n<sizeof(body)?send_text(t,body):-1;
}
int dc_transport_ping(dc_transport *t){
 if(!t)return -1;
 if(t->config.provider!=DC_TRANSPORT_XIAOZHI)return dc_db_ws_send_control(&t->db,9,(unsigned char *)"dc",2);
 if(t->ping_pending)return 0;
 if(t->ping_token==UINT64_MAX)return -1;
 uint64_t token=++t->ping_token;unsigned char payload[8];for(unsigned i=0;i<8;i++)payload[7-i]=(unsigned char)(token>>(i*8));
 if(dc_xz_ws_send_control(&t->xz,9,payload,sizeof payload))return -1;
 t->ping_pending=token;t->ping_sent_ms=milliseconds();return t->ping_sent_ms?0:-1;
}
void dc_transport_close(dc_transport *t){
 if(!t)return;
 if(t->config.provider==DC_TRANSPORT_XIAOZHI){
  if(t->ready&&t->xz.connected){char body[256];if(!xz_protocol_build_goodbye(body,sizeof(body),t->session_id))(void)send_text(t,body);}
  dc_xz_ws_close(&t->xz);
 }else {if(t->db.connected)(void)simple(t,"session.close");dc_db_ws_close(&t->db);}
 db_ogg_reset(&t->ogg);xz_opus_codec_destroy(&t->codec);
 free(t->message);free(t->decoded);free(t->text);free(t->doc);free(t);
}
