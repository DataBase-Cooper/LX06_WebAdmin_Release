/* Provider-neutral, bounded voice session. Platform HTTP/OAuth is exclusively
 * in the uploaded script (--model-request). No computer service is involved.
 * One streaming request and one bounded sentence player may run concurrently. The session-run lease encloses
 * all children; each operation has a separate cancellable process group.
 * PCM stdin is the speaker's processed mono S16LE/16 kHz microphone.
 */
#define _GNU_SOURCE
#include "../common/control_asr.h"
#include "../doubao/src/json.h"
#include "../doubao/src/text_match.h"
#include "../../hooks/robust/keeper_client.h"
#include "../../hooks/xiaoai/dialog-light.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define ANSWER_CAP 16385
#define WIRE_CAP 32768
#define HISTORY_PAIRS 3
struct pair { char question[2048],answer[3073]; };
struct job { pid_t pid; int fd; size_t used; char out[WIRE_CAP]; int64_t deadline; char cleanup[192]; };
static volatile sig_atomic_t stopped;
static void stop(int s){(void)s;stopped=1;}
static const char *env(const char*k,const char*d){const char*v=getenv(k);return v?v:d;}
static long number(const char*k,long def,long lo,long hi){
 const char*v=getenv(k);char*e;long n=v?strtol(v,&e,10):def;
 return v&&(*e||n<lo||n>hi)?def:n;
}
static void note(const char*s){fprintf(stderr,"model-voice: %s\n",s);fflush(stderr);}
static size_t cut(const char*s,size_t cap){size_t n=strlen(s);if(n<=cap)return n;n=cap;while(n&&((unsigned char)s[n]&0xc0)==0x80)n--;return n;}
static void quote(FILE*f,const char*s){
 fputc('"',f);for(const unsigned char*p=(const unsigned char*)s;*p;p++){
  if(*p=='"'||*p=='\\'){fputc('\\',f);fputc(*p,f);}else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
 }fputc('"',f);
}
/* Cancellation cannot sleep in the microphone loop. Retain the group leader
 * until the bounded TERM grace expires, then kill the whole owned group before
 * reaping it. Keeping that unreaped PID prevents a late kill from reaching a
 * reused process group. The fixed queue includes three reserved active jobs. */
#define RETIRED_CAP 32
struct retired { pid_t pid; int64_t kill_at; int killed; char cleanup[192]; };
static struct retired retired[RETIRED_CAP];
static unsigned retired_count;
static void retire_poll(int force){
 int64_t now=lk_now_ms();
 for(unsigned i=0;i<RETIRED_CAP;i++)if(retired[i].pid){
  struct retired*r=&retired[i];
  if(!r->killed&&(force||now>=r->kill_at)){kill(-r->pid,SIGKILL);r->killed=1;}
  if(!r->killed)continue;
  pid_t done=waitpid(r->pid,NULL,WNOHANG);
  if(done==r->pid||(done<0&&errno==ECHILD)){
   if(r->cleanup[0])unlink(r->cleanup);
   memset(r,0,sizeof(*r));retired_count--;
  }
 }
}
static void cancel(struct job*j){
 if(j->pid>0){
  kill(-j->pid,SIGTERM);retire_poll(0);
  unsigned i;for(i=0;i<RETIRED_CAP;i++)if(!retired[i].pid)break;
  if(i<RETIRED_CAP){
   retired[i].pid=j->pid;retired[i].kill_at=lk_now_ms()+80;
   snprintf(retired[i].cleanup,sizeof(retired[i].cleanup),"%s",j->cleanup);retired_count++;
  }else{
   /* spawn() reserves room, so exhaustion is a fatal invariant violation.
    * Stop all owned work without ever waiting on a child in the audio loop. */
   kill(-j->pid,SIGKILL);waitpid(j->pid,NULL,WNOHANG);stopped=1;
   note("cancellation capacity exhausted");
  }
 }
 if(j->fd>=0)close(j->fd);
 j->pid=0;j->fd=-1;j->used=0;j->cleanup[0]=0;
}
static int spawn(struct job*j,const char*input,const char*script,const char*audio,const char*player,int speaking){
 retire_poll(0);
 if(retired_count>=RETIRED_CAP-3){stopped=1;note("cancellation backlog; closing session");return -1;}
 int fd=open(input,O_RDONLY|O_CLOEXEC|O_NOFOLLOW),p[2];
 if(fd<0)return -1;
 if(pipe2(p,O_CLOEXEC)){close(fd);return -1;}
 pid_t parent=getpid(),pid=fork();
 if(pid<0){close(fd);close(p[0]);close(p[1]);return -1;}
 if(!pid){
  signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGPIPE,SIG_DFL);
  if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(126);
  dup2(fd,0);close(fd);close(p[0]);dup2(p[1],1);close(p[1]);
  if(!speaking){execl("/bin/sh","sh",script,"--model-request",(char*)NULL);_exit(127);}
  if(speaking==2||speaking==5){
   char wav[192];snprintf(wav,sizeof(wav),"%s.wav",input);
   int out=open(wav,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(out<0)_exit(126);
   struct rlimit limit={2*1024*1024,2*1024*1024};if(setrlimit(RLIMIT_FSIZE,&limit))_exit(126);
   dup2(out,1);close(out);execl(audio,audio,speaking==5?"--stream-synthesize":"--synthesize",(char*)NULL);_exit(127);
  }
  if(speaking==3){execl(player,player,"-q","-D","notify",(char*)NULL);_exit(127);}
  int pipefd[2];if(pipe(pipefd))_exit(126);
  pid_t synth=fork();if(synth<0)_exit(126);
  if(!synth){
   if(speaking==4){
    /* A private status descriptor carries one first-PCM marker, never PCM or
     * account material. It is separate from the WAV pipe consumed by aplay. */
    int ready=fcntl(1,F_DUPFD,10);char descriptor[24];if(ready<0)_exit(126);
    snprintf(descriptor,sizeof(descriptor),"%d",ready);setenv("MODEL_AUDIO_READY_FD",descriptor,1);
   }
   dup2(pipefd[1],1);close(pipefd[0]);close(pipefd[1]);execl(audio,audio,speaking==4?"--stream-synthesize":"--synthesize",(char*)NULL);_exit(127);
  }
  pid_t play=fork();if(play<0){kill(synth,SIGTERM);_exit(126);}
  if(!play){dup2(pipefd[0],0);close(pipefd[0]);close(pipefd[1]);execl(player,player,"-q","-D","notify",(char*)NULL);_exit(127);}
  close(pipefd[0]);close(pipefd[1]);close(0);
  int a=0,b=0;while(waitpid(synth,&a,0)<0&&errno==EINTR){};while(waitpid(play,&b,0)<0&&errno==EINTR){};
  _exit(WIFEXITED(a)&&!WEXITSTATUS(a)&&WIFEXITED(b)&&!WEXITSTATUS(b)?0:3);
 }
 close(fd);close(p[1]);setpgid(pid,pid);fcntl(p[0],F_SETFL,O_NONBLOCK);
 j->pid=pid;j->fd=p[0];j->used=0;j->deadline=lk_now_ms()+(speaking?45000:180000);
 j->cleanup[0]=0;if(speaking==2||speaking==5)snprintf(j->cleanup,sizeof(j->cleanup),"%s.wav",input);return 0;
}
/* 0 running, 1 successful, -1 failed. EOF plus exited status is required. */
static int completed(struct job*j){
 char b[2048];ssize_t n;int eof=0;
 while((n=read(j->fd,b,sizeof(b)))>0){
  if(j->used+(size_t)n>=sizeof(j->out)){cancel(j);return -1;}
  memcpy(j->out+j->used,b,(size_t)n);j->used+=(size_t)n;
 }
 if(n==0)eof=1;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){cancel(j);return -1;}
 if(lk_now_ms()>=j->deadline){cancel(j);return -1;}
 if(!eof)return 0;
 int status;pid_t done=waitpid(j->pid,&status,WNOHANG);if(done==0)return 0;
 close(j->fd);j->fd=-1;j->pid=0;j->cleanup[0]=0;j->out[j->used]=0;
 return done>0&&WIFEXITED(status)&&!WEXITSTATUS(status)?1:-1;
}
/* Incremental NDJSON reader: bounded pending line and answer, mandatory done
 * plus successful child exit. No platform-specific protocol lives in this core. */
struct stream { char answer[ANSWER_CAP],error[241]; size_t len; int done; int64_t began,first; };
static int stream_poll(struct job*j,struct stream*s){
 char b[1024];int eof=0;
 for(int budget=0;budget<8;budget++){
  ssize_t n=read(j->fd,b,sizeof(b));
  if(n==0){eof=1;break;}
  if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)break;return -1;}
  for(ssize_t i=0;i<n;i++){
   if(b[i]!='\n'){if(j->used>=4095)return -1;j->out[j->used++]=b[i];continue;}
   j->out[j->used]=0;
   xz_json_doc d;char delta[2049];int done=0;
   if(s->done||xz_json_parse(&d,j->out,j->used))return -1;
   /* Uploaded adapters own platform error wording. A typed failure is not an
    * answer: discard pending audio, speak one bounded notice, retain only
    * confirmed history, and keep the same ASR/interruption session alive. */
   if(xz_json_object_get(&d,0,"error")>=0){
    char notice[241];
    if(xz_json_get_string(&d,0,"error",notice,sizeof(notice))||!notice[0])return -1;
    for(const unsigned char*p=(const unsigned char*)notice;*p;p++)if(*p<32||*p==127)return -1;
    strcpy(s->error,notice);return -1;
   }
   if(!xz_json_get_string(&d,0,"delta",delta,sizeof(delta))){
    size_t z=strlen(delta);if(s->len+z>=ANSWER_CAP)return -1;
    memcpy(s->answer+s->len,delta,z+1);s->len+=z;
    if(z&&!s->first){s->first=lk_now_ms();fprintf(stderr,"model-voice: first delta %lld ms\n",(long long)(s->first-s->began));}
   }else if(!xz_json_get_int(&d,0,"done",&done)&&done==1){s->done=1;}
   else {int t=xz_json_object_get(&d,0,"done");
    if(t<0||d.tokens[t].end-d.tokens[t].start!=4||memcmp(d.source+d.tokens[t].start,"true",4))return -1;
    s->done=1;
   }
   j->used=0;
  }
 }
 if(lk_now_ms()>=j->deadline)return -1;
 if(!eof)return 0;
 int status;pid_t r=waitpid(j->pid,&status,WNOHANG);if(!r)return 0;
 close(j->fd);j->fd=-1;j->pid=0;
 return r>0&&WIFEXITED(status)&&!WEXITSTATUS(status)&&!j->used&&s->done&&s->len?1:-1;
}
static int save_input(const char*path,const char*text){
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
 size_t n=strlen(text);ssize_t z=write(fd,text,n);close(fd);return z==(ssize_t)n?0:-1;
}
static int request(const char*path,struct pair*h,int count,const char*q,const char*model){
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
 FILE*f=fdopen(fd,"w");if(!f){close(fd);return -1;}
 fputs("{\"values\":{\"model\":",f);quote(f,model);fputs("},\"input\":[",f);
 for(int i=0;i<count;i++){
  fputs("{\"role\":\"user\",\"content\":",f);quote(f,h[i].question);
  fputs("},{\"role\":\"assistant\",\"content\":",f);quote(f,h[i].answer);fputs("},",f);
 }
 fputs("{\"role\":\"user\",\"content\":",f);quote(f,q);fputs("}]}",f);
 long size=ftell(f);int bad=ferror(f)||size<0||size>=WIRE_CAP;
 if(fclose(f))bad=1;
 return bad?-1:0;
}
static size_t sentence(const char*s,size_t limit){
 size_t max=cut(s,limit),last=0;
 for(size_t i=0;i<max;i++){
  if(strchr(".!?;\n",s[i]))last=i+1;
  if(i+3<=max&&(!memcmp(s+i,"。",3)||!memcmp(s+i,"？",3)||!memcmp(s+i,"！",3)))last=i+3;
 }
 return last?last:max;
}
/* ASR finals arrive after playback has ended. Keep several recently spoken
 * sentences across the endpoint delay, rather than only comparing the current
 * 240-byte chunk for 800 ms (which can turn TTS echo into a new question). */
static void remember_spoken(char*recent,const char*part){
 size_t used=strlen(recent),n=strlen(part);
 if(used+n>1440){size_t drop=used+n-1440;while(drop<used&&((unsigned char)recent[drop]&0xc0)==0x80)drop++;memmove(recent,recent+drop,used-drop+1);}
 strcat(recent,part);
}
static int single_digit(const char*text){
 /* Native TTS can pronounce ASCII "2" while ASR returns the single Han
  * numeral "二". Only whole, one-digit texts are equivalent: never fold a
  * number inside a question, multiple digits, signs or decimal expressions. */
 while(*text&&(unsigned char)*text<=32)text++;
 int digit=-1;
 if(text[0]>='0'&&text[0]<='9'){digit=text[0]-'0';text++;}
 const char*han[]={"零","一","二","三","四","五","六","七","八","九"};
 if(digit<0){
  for(int i=0;i<10;i++)if(!strncmp(text,han[i],3)){digit=i;break;}
  if(!strncmp(text,"〇",3))digit=0;
  if(!strncmp(text,"两",3))digit=2;
  if(digit<0)return -1;
  text+=3;
 }
 while(*text){
  unsigned char c=(unsigned char)*text;
  if(c<128&&(c<=32||strchr(",.!?;:\"'",c))){text++;continue;}
  if(!strncmp(text,"。",3)||!strncmp(text,"，",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)||!strncmp(text,"”",3)){text+=3;continue;}
  return -1;
 }
 return digit;
}
static int exact_echo(const char*observed,const char*spoken){
 /* The shared fuzzy matcher intentionally rejects one-character texts.
  * Answers such as “二。” still need exact echo protection in this client. */
 char a[2048],b[2048];const char*in[2]={observed,spoken};char*out[2]={a,b};
 for(int k=0;k<2;k++){
  size_t n=0;const unsigned char*p=(const unsigned char*)in[k];
  while(*p&&n<2047){
   if(*p<128&&(*p<=32||strchr(",.!?;:\"'",*p))){p++;continue;}
   if(!strncmp((const char*)p,"。",3)||!strncmp((const char*)p,"，",3)||!strncmp((const char*)p,"！",3)||!strncmp((const char*)p,"？",3)||!strncmp((const char*)p,"“",3)||!strncmp((const char*)p,"”",3)){p+=3;continue;}
   out[k][n++]=(char)*p++;
  }out[k][n]=0;
 }
 if(*a&&!strcmp(a,b))return 1;
 int digit=single_digit(observed);
 return digit>=0&&digit==single_digit(spoken);
}
static int greeting_normalize(const char*text,char*out,size_t capacity){
 size_t used=0;int characters=0;
 while(*text){
  unsigned char c=(unsigned char)*text;
  if(c<128&&(c<=32||strchr(",.;:!?\"'",c))){text++;continue;}
  if(!strncmp(text,"。",3)||!strncmp(text,"，",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)||
     !strncmp(text,"；",3)||!strncmp(text,"：",3)||!strncmp(text,"、",3)||!strncmp(text,"“",3)||!strncmp(text,"”",3)){text+=3;continue;}
  if(used+1>=capacity)return -1;
  out[used++]=*text++;if((c&0xc0)!=0x80)characters++;
 }
 out[used]=0;return characters;
}
static int greeting_clause_echo(const char*observed,const char*greeting){
 /* This deliberately local rule does not widen the shared short-text fuzzy
  * matcher. Only whole configured greeting clauses (not arbitrary suffixes)
  * with at least two normalized characters can match. */
 char wanted[241],part[241],normalized[241];
 if(!*greeting||greeting_normalize(observed,wanted,sizeof(wanted))<2)return 0;
 const char*start=greeting;
 for(const char*p=greeting;;){
  size_t boundary=!*p?0:(strchr(",.;:!?\n",(unsigned char)*p)?1:
   (!strncmp(p,"。",3)||!strncmp(p,"，",3)||!strncmp(p,"！",3)||!strncmp(p,"？",3)||
    !strncmp(p,"；",3)||!strncmp(p,"：",3)||!strncmp(p,"、",3)?3:0));
  if(!*p||boundary){
   size_t length=(size_t)(p-start);if(length>=sizeof(part))return 0;
   memcpy(part,start,length);part[length]=0;
   if(greeting_normalize(part,normalized,sizeof(normalized))>=2&&!strcmp(wanted,normalized))return 1;
   if(!*p)break;
   p+=boundary;start=p;
  }else p++;
 }
 return 0;
}
/* Remove matched controls, including repeated mixtures, retaining an immediate
 * follow-up question in the same final. Uses the same normalized matching as
 * existing clients for deciding cancellation. Literal extraction is conservative. */
static void strip_controls(char*text,const char*words){
 char list[769];snprintf(list,sizeof(list),"%s",words);
 for(char*p=list;*p;p++)if(!strncmp(p,"，",3)||!strncmp(p,"、",3)){*p=',';memmove(p+1,p+3,strlen(p+3)+1);}
 char*save=NULL;
 for(char*w=strtok_r(list,",",&save);w;w=strtok_r(NULL,",",&save)){
  while(*w==' ')w++;
  size_t n=strlen(w);while(n&&w[n-1]==' ')w[--n]=0;if(!n)continue;
  char*p;while((p=strstr(text,w)))memmove(p,p+n,strlen(p+n)+1);
 }
 while(*text){
  unsigned char c=(unsigned char)*text;
  size_t n=c<=32||strchr(",.!?;",c)?1:(!strncmp(text,"，",3)||!strncmp(text,"。",3)||!strncmp(text,"！",3)||!strncmp(text,"？",3)?3:0);
  if(!n)break;
  memmove(text,text+n,strlen(text+n)+1);
 }
}
int main(int argc,char**argv){
 if(argc!=6)return 2; /* script, model-audio, aplay, model ID, matched wake */
 signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);umask(077);
 if(!strcmp(env("MODEL_TRACE_ASR","0"),"1"))setenv("LX06_CONTROL_ASR_TRACE","1",1);
 /* Keep the original 1600 ms endpoint: network-stalled partials are not proof
  * that a real spoken question has ended. Explicit 1000 ms A/B remains opt-in;
  * the shared CONTROL profile and all its callers keep their own behavior. */
 if(!getenv("LX06_QUESTION_STABLE_MS"))setenv("LX06_QUESTION_STABLE_MS","1600",0);
 const char*interrupt=env("MODEL_INTERRUPT","等一下,等等,停一下,你说的不对,你说得不对");
 const char*goodbye=env("MODEL_GOODBYE","结束对话,拜拜了哦");
 long any=number("MODEL_BARGE_ANY",0,0,1),idle=number("MODEL_IDLE_SECONDS",120,1,3600);
 /* A/B only: never change the established recognition path by default. */
 int tts_yield=number("MODEL_TTS_YIELD",0,0,1),tts_held=0;
 /* Uploaded adapters opt in only after the streaming transport is accepted.
  * Its first PCM goes directly to our player; ASR always keeps its slot. */
 int stream_tts=number("MODEL_TTS_STREAM",0,0,1);
 if(stream_tts)tts_yield=0;
 /* Script-owned policy; other clients retain their existing segmentation. */
 int smooth=!strcmp(env("MODEL_SPEECH_FLOW","首声优先"),"连贯优先");
 int64_t tts_until=0;
 char dir[]="/tmp/lx06-model-voice.XXXXXX",input[128],speech[128],wav[192];if(!mkdtemp(dir))return 3;
 snprintf(input,sizeof(input),"%s/input",dir);snprintf(speech,sizeof(speech),"%s/speech",dir);snprintf(wav,sizeof(wav),"%s.wav",speech);
 lx06_control_asr*asr=lx06_control_asr_open_profile(getenv("LX06_CONTROL_ASR_SOCKET"),LX06_ASR_QUESTION);
 struct job job={.fd=-1},net={.fd=-1},synth={.fd=-1};struct stream stream={0};int received=1,prepared=0,played_first=0;struct pair history[HISTORY_PAIRS];int count=0,phase=0,rc=0;
 char answer[ANSWER_CAP]={0},question[2048]={0},next_question[2048]={0},spoken[241]={0},pending[241]={0},recent[1441]={0},greeting[241]={0},streaming_text[241]={0};size_t offset=0;
 int stream_start_pending=0;
 uint64_t generation=0,active=0,greeting_generation=0,synthesis_serial=0;int partial_seen=0,greeting_partial_play=0;
 char traced_partial[2048]={0};
 int64_t last=lk_now_ms(),started=last,next=last,echo_until=0;
 int64_t chunk_since=0,play_started=0,play_ended=0;unsigned play_chunks=0;
 if(!asr){note("ASR unavailable");rc=3;goto done;}
 if(tts_yield && lx06_control_asr_tts_priority_enable(asr)!=1){tts_yield=0;note("TTS priority unavailable; recognition retained");}
 fprintf(stderr,"model-voice: question endpoint %s ms\n",env("LX06_QUESTION_STABLE_MS","1600"));
 int flags=fcntl(0,F_GETFL);if(flags<0||fcntl(0,F_SETFL,flags|O_NONBLOCK)){rc=3;goto done;}
 unsigned char pcm[640];size_t used=0;
 snprintf(answer,sizeof(answer),"%s",env("MODEL_GREETING","我在呢，请说。"));
 if(strlen(answer)<sizeof(greeting))strcpy(greeting,answer);
 if(*answer){phase=2;active=++generation;greeting_generation=active;}else lx06_dialog_light(LX06_LIGHT_LISTENING);
 note("ready");
 while(!stopped){
  int64_t now=lk_now_ms();
  retire_poll(0);
  if(!phase&&now-last>idle*1000)break;
  if(tts_held&&(!synth.pid||now>=tts_until)){
   if(lx06_control_asr_tts_resume(asr,(uint64_t)now)){rc=3;break;}
   tts_held=0;note("TTS yield resumed");
  }
  if(now>=next){
   ssize_t n=read(0,pcm+used,sizeof(pcm)-used);
   if(n>0)used+=(size_t)n;
   else if(!n){note("microphone closed");break;}
   else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){rc=3;break;}
   lx06_control_asr_reply(asr,active);
   if(used==sizeof(pcm)){
    if(lx06_control_asr_feed(asr,1,pcm,used,now)){fprintf(stderr,"model-voice: ASR feed failed errno=%d\n",errno);rc=3;break;}used=0;next+=20;if(next<now-100)next=now;
   }else if(lx06_control_asr_feed(asr,1,NULL,0,now)){fprintf(stderr,"model-voice: ASR feed failed errno=%d\n",errno);rc=3;break;}
  }
  char text[2048];int result=lx06_control_asr_poll(asr,text,sizeof(text),now);
  if(result<0){note("ASR disconnected");rc=3;break;}
  /* A done frame may precede the request child's cleanup exit. Once its
   * audio is drained, accept one bounded final while cleanup verifies success;
   * do not cancel that cleanup merely because the next utterance starts. */
  int cleanup_only=phase==1&&!received&&stream.done&&!answer[offset]&&!job.pid&&!synth.pid&&!prepared;
  uint64_t partial_reply;const char*partial=lx06_control_asr_partial(asr,&partial_reply);
  /* Observe the first partial only while its original ASR utterance is live.
   * A partial that began before playback is never promoted into this rule.
   * If partial+final arrive in one poll, no observation is invented afterward. */
  if(*partial&&!partial_seen){
   partial_seen=1;greeting_partial_play=phase==2&&job.pid&&partial_reply==greeting_generation;
  }else if(!*partial&&!result){partial_seen=0;greeting_partial_play=0;}
  int partial_control=xz_text_matches_list(partial,interrupt)||xz_text_matches_list(partial,goodbye);
  int partial_self_control=xz_self_echo(partial,recent,interrupt,goodbye,"");
  /* A recognizer can prepend leaked playback to the user's command. A fuzzy
   * match of that shared prefix must not hide a NEW control word absent from
   * our playback. Controls actually spoken by us retain self-echo protection. */
  int partial_echo=active&&!cleanup_only&&partial_reply==active&&*partial&&now<echo_until&&
   (exact_echo(partial,spoken)||(!partial_control&&xz_text_is_echo(partial,recent))||partial_self_control||
   (greeting_partial_play&&greeting_generation&&partial_reply==greeting_generation&&greeting_clause_echo(partial,greeting)));
  /* Explicit diagnostic mode is enabled only for a bounded synthetic test.
   * Log changed hypotheses, not every microphone frame or account material. */
  if(!strcmp(env("MODEL_TRACE_ASR","0"),"1")&&strcmp(traced_partial,partial)){
   snprintf(traced_partial,sizeof(traced_partial),"%s",partial);
   fprintf(stderr,"model-voice: asr-partial at=%lld reply=%llu active=%llu phase=%d echo=%d player=%ld text=",
    (long long)now,(unsigned long long)partial_reply,(unsigned long long)active,phase,partial_echo,
    (long)job.pid);
   quote(stderr,partial);fputc('\n',stderr);
  }
  /* A complete configured control is enough to silence our own reply now.
   * Do NOT end, cancel, restart or replay ASR: the same live utterance must
   * retain the user's words after the control until its genuine FINAL. The
   * old control-hint endpoint could finalize in the middle of that follow-up.
   * Only FINAL becomes a model request. Clearing active deduplicates repeated
   * or mixed controls; the utterance's original reply tag guards newer replies. */
  lx06_control_asr_control_hint(asr,0);
  if(active&&!cleanup_only&&partial_reply==active&&*partial&&!partial_echo&&(any||partial_control)){
   cancel(&job);cancel(&net);cancel(&synth);unlink(wav);prepared=0;stream_start_pending=0;phase=0;active=0;answer[0]=0;next_question[0]=0;last=now;echo_until=now+5000;
   lx06_dialog_light(LX06_LIGHT_INTERRUPTED);lx06_dialog_light(LX06_LIGHT_LISTENING);
   note(any?"human interruption":(xz_text_matches_list(partial,goodbye)?"goodbye pending":"keyword interruption"));
  }
  if(result){
   uint64_t tagged=lx06_control_asr_result_reply(asr);
   if(!strcmp(env("MODEL_TRACE_ASR","0"),"1")){fputs("model-voice: asr-final ",stderr);quote(stderr,text);fputc('\n',stderr);}
   int final_control=xz_text_matches_list(text,interrupt)||xz_text_matches_list(text,goodbye);
   int echo=now<echo_until&&(exact_echo(text,spoken)||(!final_control&&xz_text_is_echo(text,recent))||xz_self_echo(text,recent,interrupt,goodbye,"")||
    (greeting_partial_play&&greeting_generation&&tagged==greeting_generation&&greeting_clause_echo(text,greeting)));
   partial_seen=0;greeting_partial_play=0;
   if(!echo){
    /* The routing phrase can still be in the mic ring when ownership starts.
     * Do not send the wake itself as the first model question. */
    if(now-started<8000&&argv[5][0]&&exact_echo(text,argv[5])){note("wake phrase consumed");last=now;lx06_dialog_light(LX06_LIGHT_LISTENING);poll(NULL,0,5);continue;}
    if(xz_text_matches_list(text,goodbye)){note("goodbye");break;}
    int control=xz_text_matches_list(text,interrupt);
    if(control){
     next_question[0]=0;
     if(active&&tagged==active){cancel(&job);cancel(&net);cancel(&synth);unlink(wav);prepared=0;stream_start_pending=0;phase=0;active=0;answer[0]=0;echo_until=now+5000;lx06_dialog_light(LX06_LIGHT_INTERRUPTED);note("keyword interruption");}
     strip_controls(text,interrupt);
     /* A punctuation-split control that cannot be safely extracted must not
      * become a model question, or repeated controls would create replies. */
     if(xz_text_matches_list(text,interrupt))text[0]=0;
     /* Echo can precede a newly spoken command in one hypothesis. Once the
      * command is stripped, don't submit a pure playback prefix as a question. */
     if(*text&&now<echo_until&&(exact_echo(text,spoken)||xz_text_is_echo(text,recent)))text[0]=0;
    }
    if(!phase||(!control&&cleanup_only)){
     last=now;
     if(*text){
      /* The first final owns the one-slot queue. Additional finals during a
       * slow cleanup are merged within the same existing question-size cap. */
      size_t used=strlen(next_question),n=strlen(text);
      if(used+n+(used?1:0)<sizeof(next_question)){
       if(used)strcat(next_question," ");strcat(next_question,text);
       if(phase)note("follow-up queued during request cleanup");
      }else note("follow-up exceeds bounded question queue");
     }else if(!phase)lx06_dialog_light(LX06_LIGHT_LISTENING);
    }
   }
  }
  if(net.pid){
   int ready=stream_poll(&net,&stream);
   if(ready<0){
    cancel(&net);cancel(&job);cancel(&synth);unlink(wav);prepared=0;stream_start_pending=0;note("stream failed; pending audio discarded");phase=0;active=0;answer[0]=0;last=now;echo_until=now+5000;lx06_dialog_light(LX06_LIGHT_LISTENING);
    if(stream.error[0]){
     snprintf(answer,sizeof(answer),"%s",stream.error);offset=0;received=1;
     chunk_since=0;play_chunks=0;play_started=play_ended=0;phase=3;active=++generation;
     note("provider error notice");
    }
   }else{
    memcpy(answer,stream.answer,stream.len+1);
    if(ready){
     received=1;note("answer");
     fprintf(stderr,"model-voice: complete %lld ms\n",(long long)(now-stream.began));
     if(count==HISTORY_PAIRS){memmove(history,history+1,(HISTORY_PAIRS-1)*sizeof(*history));count--;}
     strcpy(history[count].question,question);size_t n=cut(answer,sizeof(history[count].answer)-1);memcpy(history[count].answer,answer,n);history[count++].answer[n]=0;
    }
   }
  }
  if(job.pid){
   int ready=completed(&job);
   if(stream_start_pending&&memchr(job.out,1,job.used)){
    stream_start_pending=0;play_started=now;strcpy(spoken,streaming_text);
    if(now>=echo_until)recent[0]=0;remember_spoken(recent,spoken);echo_until=now+50000;
    lx06_dialog_light(LX06_LIGHT_SPEAKING);note("speaking");
    if(!stream.began)fprintf(stderr,"model-voice: greeting first PCM %lld ms\n",(long long)(now-started));
    else if(!played_first){played_first=1;fprintf(stderr,"model-voice: first PCM %lld ms\n",(long long)(now-stream.began));}
   }
   if(ready&&stream_start_pending){stream_start_pending=0;note("stream ended without first PCM marker");}
   if(ready<0){cancel(&net);cancel(&synth);unlink(wav);prepared=0;note("operation failed");phase=0;active=0;answer[0]=0;last=now;echo_until=now+5000;lx06_dialog_light(LX06_LIGHT_LISTENING);}
   else if(ready){play_ended=now;fprintf(stderr,"model-voice: playback ended chunk=%u duration_ms=%lld\n",play_chunks,(long long)(now-play_started));}
  }
  if(synth.pid){
   int ready=completed(&synth);
   if(ready<0){cancel(&net);cancel(&job);unlink(wav);prepared=0;stream_start_pending=0;phase=0;active=0;answer[0]=0;last=now;echo_until=now+5000;note("operation failed");lx06_dialog_light(LX06_LIGHT_LISTENING);}
   else if(ready){
    /* Release this completed operation before starting playback/prefetch.
     * The next synthesis must explicitly reserve the remaining bounded
     * window; the transport refuses to extend that original deadline. */
    if(tts_held){
     if(lx06_control_asr_tts_resume(asr,(uint64_t)now)){rc=3;break;}
     tts_held=0;note("TTS yield resumed");
    }
    prepared=1;
   }
  }
  if(phase){
   /* One current PCM file and at most one prefetched sentence. Start synthesis
    * during playback to hide network TTS latency without buffering long audio. */
   if(!job.pid&&prepared){
    if(spawn(&job,wav,argv[1],argv[2],argv[3],3)){rc=3;break;}
    play_started=now;play_chunks++;
    fprintf(stderr,"model-voice: playback chunk=%u gap_ms=%lld text_bytes=%zu\n",play_chunks,(long long)(play_ended?now-play_ended:0),strlen(pending));
    unlink(wav);prepared=0;strcpy(spoken,pending);
    if(now>=echo_until)recent[0]=0;
    remember_spoken(recent,spoken);echo_until=now+50000;lx06_dialog_light(LX06_LIGHT_SPEAKING);note("speaking");
    if(!offset||!stream.began)fprintf(stderr,"model-voice: greeting playback %lld ms\n",(long long)(now-started));
    else if(!played_first){played_first=1;fprintf(stderr,"model-voice: first playback %lld ms\n",(long long)(now-stream.began));}
   }
   if(!synth.pid&&!prepared&&answer[offset]){
     /* Early-speech mode can start at a comma. Continuous mode batches tiny
      * phrases to cover the next native TTS request without cancelling ASR.
      * Limits below count UTF-8 bytes, never split a code point, and preserve
      * the existing one-playing/one-prefetched WAV memory bound. */
    if(!chunk_since)chunk_since=now;
    int flowing=smooth&&phase==1&&!(stream_tts&&!offset);
    size_t limit=(!offset&&phase==1&&!flowing)?72:240;
    size_t n=sentence(answer+offset,limit),available=strlen(answer+offset);
    if(!offset&&phase==1&&!flowing)for(size_t i=0;i<n;i++) {
     if((answer[offset+i]==','&&i>=18)||(i>=18&&i+3<=n&&!memcmp(answer+offset+i,"，",3))){n=i+(answer[offset+i]==','?1:3);break;}
    }
    int boundary=n&&((unsigned char)answer[offset+n-1]<128&&strchr(".!?;\n",answer[offset+n-1]));
    if(n>=3&&(!memcmp(answer+offset+n-3,"。",3)||!memcmp(answer+offset+n-3,"？",3)||!memcmp(answer+offset+n-3,"！",3)))boundary=1;
    /* Merge tiny comma/sentence fragments so playback covers the next native
     * synthesis. Keep one prefetched WAV and continuous ASR. Finished answers
     * flush immediately, sparse streams have a bounded batching delay. */
    int emit=boundary||available>=limit||(available>=18&&stream.first&&now-stream.first>=250);
    if(flowing){
     /* A 12-second phrase still underruns an observed 13.4-second native
      * synthesis retry. Use the existing full cap, rather than more WAVs.
      * Sparse deltas may not fill it: bound that extra wait to two seconds. */
     emit=available>=240||(n>=216&&now-chunk_since>=700)||(available>=24&&now-chunk_since>=2000);
     if(n<216&&(available>=240||received||stream.done))n=cut(answer+offset,limit);
    }
    if(received||stream.done||emit){
     if(!offset&&stream.began)fprintf(stderr,"model-voice: first synthesis %lld ms\n",(long long)(now-stream.began));
     /* Give only the FIRST, not-yet-playing phrase a bounded head start.
      * Never cancel an observed user utterance or hand ASR away during
      * playback/prefetch: either could delay/drop an interruption. Weak PCM
      * without a partial is still retained in the original encoded replay. */
     if(tts_yield&&phase==1&&!played_first&&!*partial){
       int yielded=lx06_control_asr_tts_priority(asr,(uint64_t)now,500);
       if(yielded<0){rc=3;break;}
       if(yielded){tts_held=1;tts_until=now+500;note("TTS priority before synthesis");}
     }
     memcpy(pending,answer+offset,n);pending[n]=0;offset+=n;
     chunk_since=0;
     /* A canceled synthesizer may still be within its TERM grace. Unique
      * paths keep its late writes away from the next reply's bounded WAV. */
     snprintf(speech,sizeof(speech),"%s/speech-%llu",dir,(unsigned long long)++synthesis_serial);
     snprintf(wav,sizeof(wav),"%s.wav",speech);
     if(save_input(speech,pending)){rc=3;break;}
     if(stream_tts&&!job.pid&&play_chunks==0){
      /* First sound does not wait for a complete WAV. The audio helper owns
       * its bounded native FIFO drainer; canceling this group stops only our
       * audible pipeline. Further chunks retain one-file prefetch, using the
       * same streaming synthesis transport to avoid the stock save-file wait. */
      if(spawn(&job,speech,argv[1],argv[2],argv[3],4)){rc=3;break;}
      play_started=now;play_chunks++;stream_start_pending=1;strcpy(streaming_text,pending);
      fprintf(stderr,"model-voice: playback chunk=%u gap_ms=0 text_bytes=%zu stream=1\n",play_chunks,strlen(pending));
      /* Keep THINKING and do not claim echo text until first decoded PCM is
       * reported through the private status pipe. Pipeline launch is not sound. */
     }else if(spawn(&synth,speech,argv[1],argv[2],argv[3],stream_tts?5:2)){rc=3;break;}
     unlink(speech); /* spawn() already holds the private input descriptor. */
    }
   }
   if(received&&!answer[offset]&&!job.pid&&!synth.pid&&!prepared){phase=0;active=0;last=now;offset=0;echo_until=now+5000;lx06_dialog_light(LX06_LIGHT_LISTENING);note("listening");}
  }
  /* Context is committed only by successful stream_poll above. A failed old
   * request still releases this queued final, using only confirmed history. */
  if(!phase&&next_question[0]){
   snprintf(question,sizeof(question),"%s",next_question);next_question[0]=0;
   if(request(input,history,count,question,argv[4])||spawn(&net,input,argv[1],argv[2],argv[3],0)){rc=3;break;}
   memset(&stream,0,sizeof(stream));stream.began=now;received=0;played_first=0;answer[0]=0;offset=0;chunk_since=0;play_chunks=0;play_started=play_ended=0;phase=1;active=++generation;lx06_dialog_light(LX06_LIGHT_THINKING);note("request");
  }
  poll(NULL,0,5);
 }
done:
 cancel(&net);cancel(&job);cancel(&synth);
 /* One total shutdown bound, rather than a blocking wait for every child. */
 int64_t close_until=lk_now_ms()+250;
 do{retire_poll(1);if(!retired_count)break;poll(NULL,0,5);}while(lk_now_ms()<close_until);
 lx06_control_asr_close(asr);unlink(input);unlink(speech);unlink(wav);rmdir(dir);lx06_dialog_light(LX06_LIGHT_END);note("closed");return stopped?0:rc;
}
