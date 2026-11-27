/* Offline production dispatcher -> Unix seqpacket -> production core gate.
 * This is not the full core reactor or a live transport/device test. */
#define _GNU_SOURCE
#ifdef NDEBUG
#error assertions must be enabled
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "../phone.c"
#include "../../mode_gate.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void inbound(struct phone *w, unsigned type, uint64_t epoch, const char *json) {
    dc_wire_packet p = {0}; size_t n=strlen(json);
    p.header=dc_wire_make(type,epoch,0,0,n); memcpy(p.payload,json,n);
    CHECK(remote_packet(w,(const unsigned char *)&p,sizeof p.header+n)==0);
}
static const char status[]="{\"probe_only\":false,\"call_ready\":true,\"capabilities\":{\"virtual_mic\":true,\"rtc_capture\":true,\"call_lifecycle\":true,\"reply_lifecycle\":true,\"turn_fence\":false,\"automatic_ui\":false}}";
static void send_ack(struct phone *w,int replay,uint64_t epoch) {
    char json[320]; int n=snprintf(json,sizeof json,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"%llu\",\"command_id\":\"%s\",\"status\":\"completed\",\"code\":\"ok\",\"replayed\":%s}",(unsigned long long)epoch,w->command,replay?"true":"false");
    CHECK(n>0 && (size_t)n<sizeof json);inbound(w,DC_EVT_TEXT,epoch,json);
}
static void completed_then_old_started(void) {
    int sv[2]; CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,sv)==0);
    dc_mode_request request={0}; strcpy(request.script,"phone_doubao.sh");strcpy(request.target,"doubao");strcpy(request.action,"call.start");strcpy(request.source,"local_wake");strcpy(request.wake,"test");
    dc_phone_gate gate;char envelope[1024];CHECK(dc_mode_phone_open(&request,123,1793494800000ULL,100,&gate,envelope,sizeof envelope)>0);
    struct phone *w=calloc(1,sizeof *w);CHECK(w);w->core=sv[0];w->socket=-1;w->epoch=123;strcpy(w->command,gate.command_id);
    inbound(w,DC_EVT_READY,123,status);
    send_ack(w,0,123);
    char old[320];int n=snprintf(old,sizeof old,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"123\",\"command_id\":\"%s\",\"status\":\"started\",\"code\":\"accepted\",\"replayed\":true}",w->command);
    CHECK(n>0 && (size_t)n<sizeof old);inbound(w,DC_EVT_TEXT,123,old);
    CHECK(local_flush(w)==0);
    dc_wire_packet p;int r,ready=0,count=0;while((r=dc_wire_receive(sv[1],&p))==1){int accepted=dc_mode_phone_event(&gate,&p,101);CHECK(accepted>=0);ready+=accepted;count++;}
    CHECK(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK));CHECK(count>=3);CHECK(ready==1);CHECK(!gate.terminal && !gate.replayed && gate.ack_state==2);
    /* A non-replayed regression must still fail closed. */
    n=snprintf(old,sizeof old,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"123\",\"command_id\":\"%s\",\"status\":\"started\",\"code\":\"accepted\",\"replayed\":false}",w->command);
    CHECK(n>0 && (size_t)n<sizeof old);dc_wire_packet bad={0};bad.header=dc_wire_make(DC_EVT_TEXT,123,0,0,(size_t)n);memcpy(bad.payload,old,(size_t)n);
    CHECK(remote_packet(w,(const unsigned char *)&bad,sizeof bad.header+(size_t)n)<0);CHECK(w->completed && w->ack_state==2);
    close(sv[0]);close(sv[1]);free(w);
}
static void scenario(int first_replay) {
    int sv[2]; CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,sv)==0);
    dc_mode_request request={0}; strcpy(request.script,"phone_doubao.sh");strcpy(request.target,"doubao");strcpy(request.action,"call.start");strcpy(request.source,"local_wake");strcpy(request.wake,"test");
    dc_phone_gate gate;char envelope[1024];CHECK(dc_mode_phone_open(&request,123,1793494800000ULL,100,&gate,envelope,sizeof envelope)>0);
    struct phone *w=calloc(1,sizeof *w);CHECK(w);w->core=sv[0];w->socket=-1;w->epoch=123;strcpy(w->command,gate.command_id);
    send_ack(w,0,124); /* Stale session must never enter the local queue. */
    send_ack(w,first_replay,123);send_ack(w,!first_replay,123);inbound(w,DC_EVT_READY,123,status);CHECK(local_flush(w)==0);
    dc_wire_packet p;int count=0,ready=0,r;
    while((r=dc_wire_receive(sv[1],&p))==1){
        count++; int result=dc_mode_phone_event(&gate,&p,101);
        if(first_replay){CHECK(result==0);CHECK(!gate.terminal);CHECK(gate.replayed);}
        else {CHECK(result>=0);if(result==1)ready++;}
    }
    CHECK(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    CHECK(count==(first_replay?3:4));CHECK(ready==(first_replay?0:1));
    if(first_replay){CHECK(gate.replayed);CHECK(!w->ready);}
    else CHECK(gate.ack_state==2 && !gate.terminal);
    close(sv[0]);close(sv[1]);free(w);
}
int main(void){scenario(0);scenario(1);completed_then_old_started();puts("dispatcher-to-core replay metadata: PASS");return 0;}
