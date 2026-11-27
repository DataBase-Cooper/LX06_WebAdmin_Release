/* Explicit idle-device probe. PCM s16le/16k/mono on stdin; no microphone access. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <opus/opus.h>
#include "../../runtime/doubao/src/json.h"
#include "../../runtime/common/control_asr.h"

static int continuous(void) {
 lx06_control_asr *a=lx06_control_asr_open(NULL);if(!a)return 2;
 unsigned char pcm[640];char text[2048];int status=0;
 for(int frames=0;frames<3000;frames++) {
  size_t n=fread(pcm,1,sizeof(pcm),stdin);if(!n)break;
  memset(pcm+n,0,sizeof(pcm)-n);
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  uint64_t ms=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
  int got=lx06_control_asr_poll(a,text,sizeof(text),ms);
  if(got<0 || lx06_control_asr_feed(a,1,pcm,sizeof(pcm),ms)){status=1;break;}
  if(got>0)printf("final=%s\n",text);
  struct timespec delay={0,20000000};nanosleep(&delay,NULL);
 }
 lx06_control_asr_close(a);return status;
}

static int final_seen;
static char dialog[96];
static int receive_event(int fd,int timeout) {
 struct pollfd p={fd,POLLIN,0};if(poll(&p,1,timeout)<=0)return 0;
 char data[65536];ssize_t n=recv(fd,data,sizeof(data)-1,MSG_TRUNC);
 if(n<=0||n>=(ssize_t)sizeof(data))return -1;
 data[n]=0;
 xz_json_doc *d=calloc(1,sizeof(*d));if(!d)return -1;
 if(xz_json_parse(d,data,(size_t)n)){free(d);return -1;}
 char type[64];
 if(!xz_json_get_string(d,d->root,"type",type,sizeof(type))) {
  if(!strcmp(type,"ready")) {
   int result=xz_json_get_string(d,d->root,"dialog",dialog,sizeof(dialog));free(d);return result?-1:2;
  }
  printf("bridge=%s\n",type);free(d);return !strcmp(type,"error")?-1:1;
 }
 int h=xz_json_object_get(d,d->root,"header"),body=xz_json_object_get(d,d->root,"payload");
 char id[96],ns[64],name[64];
 if(xz_json_get_string(d,h,"dialog_id",id,sizeof(id)) || strcmp(id,dialog) ||
    xz_json_get_string(d,h,"namespace",ns,sizeof(ns)) || xz_json_get_string(d,h,"name",name,sizeof(name))){free(d);return 1;}
 printf("event=%s.%s\n",ns,name);
 if(!strcmp(ns,"SpeechRecognizer")&&!strcmp(name,"RecognizeResult")) {
  int fin=xz_json_object_get(d,body,"is_final"),results=xz_json_object_get(d,body,"results");
  if(fin>=0 && d->tokens[fin].end-d->tokens[fin].start==4 && !memcmp(data+d->tokens[fin].start,"true",4) &&
     results>=0 && d->tokens[results].type==XZ_JSON_ARRAY) {
   char text[1024];if(!xz_json_get_string(d,d->tokens[results].first_child,"text",text,sizeof(text))) {
    printf("final=%s\n",text);final_seen=1;
   }
  }
 }
 free(d);return 1;
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);
 if(argc>1 && !strcmp(argv[1],"continuous"))return continuous();
 int framed=argc<2 || strcmp(argv[1],"raw"),result=1;
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0)return 2;
 struct sockaddr_un addr={.sun_family=AF_UNIX};strcpy(addr.sun_path,"/tmp/lx06-control-asr-probe.sock");
 if(connect(fd,(void*)&addr,sizeof(addr))){perror("bridge connect");close(fd);return 2;}
 if(receive_event(fd,2000)<0 || send(fd,argc>1&&!strcmp(argv[1],"vad")?"V":"S",1,MSG_NOSIGNAL)!=1)goto end;
 for(int i=0;i<10&&!dialog[0];i++)if(receive_event(fd,500)<0)goto end;
 if(!*dialog){puts("not ready");goto end;}
 int error=0;OpusEncoder *enc=opus_encoder_create(16000,1,OPUS_APPLICATION_VOIP,&error);
 if(!enc||error)goto end;
 opus_encoder_ctl(enc,OPUS_SET_BITRATE(24000));opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(3));
 short pcm[320];unsigned char packet[4097];
 for(int frames=0;frames<750;frames++) {
  size_t n=fread(pcm,1,sizeof(pcm),stdin);if(!n)break;
  memset((unsigned char*)pcm+n,0,sizeof(pcm)-n);
  int offset=framed?3:1;
  int bytes=opus_encode(enc,pcm,320,packet+offset,(opus_int32)sizeof(packet)-offset);
  if(bytes<=0){opus_encoder_destroy(enc);goto end;}
  packet[0]='A';if(framed){packet[1]=(unsigned char)(bytes>>8);packet[2]=(unsigned char)bytes;}
  if(send(fd,packet,(size_t)bytes+offset,MSG_NOSIGNAL)!=bytes+offset){opus_encoder_destroy(enc);goto end;}
  while(receive_event(fd,0)>0){}
  struct timespec pause={0,20000000};nanosleep(&pause,NULL);
 }
 opus_encoder_destroy(enc);
 if(send(fd,"E",1,MSG_NOSIGNAL)!=1)goto end;
 for(int i=0;i<32&&!final_seen;i++)if(receive_event(fd,250)<0)break;
 result=final_seen?0:1;
end:
 (void)send(fd,"C",1,MSG_NOSIGNAL);close(fd);return result;
}
