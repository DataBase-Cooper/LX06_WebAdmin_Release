/* Synthetic remote boundary around the actual phone packet dispatcher.
 * Only FD 3, an inherited AF_UNIX seqpacket, is used; never opens a network. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../phone.c"
static int dispatch(struct phone *w,unsigned type,uint64_t epoch,const char *text){
 dc_wire_packet p={0};size_t n=strlen(text);p.header=dc_wire_make(type,epoch,0,0,n);memcpy(p.payload,text,n);return remote_packet(w,(const unsigned char *)&p,sizeof p.header+n);
}
static int response(struct phone *w,int replay,uint64_t epoch){
 char json[320];int n=snprintf(json,sizeof json,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"%llu\",\"command_id\":\"%s\",\"status\":\"completed\",\"code\":\"ok\",\"replayed\":%s}",(unsigned long long)epoch,w->command,replay?"true":"false");return n<0||(size_t)n>=sizeof json?-1:dispatch(w,DC_EVT_TEXT,epoch,json);
}
int main(void){
 struct phone *w=calloc(1,sizeof *w);if(!w)return 2;w->core=3;w->socket=-1;
 int flags=fcntl(3,F_GETFL);if(flags<0||fcntl(3,F_SETFL,flags&~O_NONBLOCK))return 7;
 dc_wire_packet open;struct command cmd;struct pollfd opening={3,POLLIN,0};if(poll(&opening,1,5000)<=0||dc_wire_receive(3,&open)!=1||open.header.type!=DC_CMD_OPEN||open_envelope(w,&open,&cmd))return 3;
 w->epoch=open.header.epoch;strcpy(w->command,cmd.id);const char *s=getenv("DC_REPLAY_ORDER");if(!s)return 4;int historical=!strcmp(s,"historical");
 usleep(600000);
 const char *status="{\"probe_only\":false,\"call_ready\":true,\"capabilities\":{\"virtual_mic\":true,\"rtc_capture\":true,\"call_lifecycle\":true,\"reply_lifecycle\":true,\"turn_fence\":false,\"automatic_ui\":false}}";
 if(!strcmp(s,"genuine_old")){
  char old[320];int n=snprintf(old,sizeof old,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"%llu\",\"command_id\":\"%s\",\"status\":\"started\",\"code\":\"accepted\",\"replayed\":true}",(unsigned long long)w->epoch,w->command);
  if(n<0||(size_t)n>=sizeof old||dispatch(w,DC_EVT_READY,w->epoch,status)||response(w,0,w->epoch)||dispatch(w,DC_EVT_TEXT,w->epoch,old)||response(w,1,w->epoch)||local_flush(w))return 5;
 }else if(response(w,0,w->epoch+1)||response(w,historical,w->epoch)||response(w,!historical,w->epoch)||dispatch(w,DC_EVT_READY,w->epoch,status)||local_flush(w))return 5;
 dc_wire_packet p;struct pollfd poller={3,POLLIN,0};if(poll(&poller,1,15000)<=0)return 6;int r=dc_wire_receive(3,&p);if(r!=1||p.header.type!=DC_CMD_CLOSE)return 6;
 free(w);return 0;
}
