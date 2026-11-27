/* Complete-question ASR probe/primitive. PCM stdin: mono S16LE, 16 kHz.
 * Run inside session-run so the existing SDK bridge binds its owner lease.
 * Uses this speaker's official authentication, not a copied device credential.
 * Reads in real time, emits one final JSON text and exits; never promotes a
 * partial hypothesis to a question. No transcript goes to stderr/system logs.
 */
#define _GNU_SOURCE
#include "../common/control_asr.h"
#include "../../hooks/robust/keeper_client.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
static volatile sig_atomic_t stopped;
static void stop(int sig){(void)sig;stopped=1;}
static void quote(const char*s){
 putchar('"');
 for(const unsigned char*p=(const unsigned char*)s;*p;p++){
  if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}
  else if(*p<32)printf("\\u%04x",*p);
  else putchar(*p);
 }
 putchar('"');
}
int main(int argc,char**argv){
 if(argc!=2||strcmp(argv[1],"--once"))return 2;
 signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
 lx06_control_asr*a=lx06_control_asr_open_profile(getenv("LX06_CONTROL_ASR_SOCKET"),LX06_ASR_QUESTION);
 if(!a){fputs("model-asr: service unavailable\n",stderr);return 3;}
 int flags=fcntl(0,F_GETFL);if(flags<0||fcntl(0,F_SETFL,flags|O_NONBLOCK)){lx06_control_asr_close(a);return 3;}
 unsigned char pcm[640];size_t used=0;int eof=0,rc=3;
 int64_t begin=lk_now_ms(),next=begin,eof_at=0;
 fputs("model-asr: ready\n",stderr);
 while(!stopped&&lk_now_ms()-begin<60000){
  int64_t now=lk_now_ms();
  if(now>=next){
   if(!eof&&used<sizeof(pcm)){
    ssize_t n=read(0,pcm+used,sizeof(pcm)-used);
    if(n>0)used+=(size_t)n;
    else if(!n){eof=1;eof_at=now;}
    else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)break;
   }
   if(used==sizeof(pcm)||eof){
    if(used%2)break;
    memset(pcm+used,0,sizeof(pcm)-used);
    if(lx06_control_asr_feed(a,1,pcm,sizeof(pcm),(uint64_t)now))break;
    used=0;next=now+20;
   }else if(lx06_control_asr_feed(a,1,NULL,0,(uint64_t)now))break;
  }
  char text[2048];int result=lx06_control_asr_poll(a,text,sizeof(text),(uint64_t)now);
  if(result<0)break;
  if(result){fputs("{\"text\":",stdout);quote(text);puts("}");rc=0;break;}
  if(eof&&now-eof_at>10000)break;
  poll(NULL,0,5);
 }
 fcntl(0,F_SETFL,flags);lx06_control_asr_close(a);
 return stopped?130:rc;
}
