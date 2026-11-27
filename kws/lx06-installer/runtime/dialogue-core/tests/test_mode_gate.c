#define _GNU_SOURCE
#include "../mode_gate.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"mode-gate-failed line=%u\n",__LINE__);exit(1);}}while(0)
static const char phone[]="{\"v\":1,\"script_id\":\"phone_doubao.sh\",\"wake_source\":\"official_asr\",\"wake_text\":\"给豆包打电话\",\"target\":\"doubao\",\"action\":\"call.start\"}";
static const char action[]="{\"v\":1,\"script_id\":\"door_open.sh\",\"wake_source\":\"local_wake\",\"wake_text\":\"打开玻璃门\",\"action\":\"door.open\",\"status\":\"success\"}";
static dc_phone_gate gate(void){dc_mode_request r;dc_phone_gate g;char output[1024];CHECK(!dc_mode_parse_request(DC_MODE_PHONE,phone,strlen(phone),&r));CHECK(dc_mode_phone_open(&r,77,1793494800000ULL,100,&g,output,sizeof output)>0);CHECK(strstr(output,"\"deadline_unix_ms\":\"1793494810000\""));CHECK(strstr(output,"给豆包打电话"));return g;}
static dc_wire_packet packet(unsigned type,const char *text){dc_wire_packet p={0};p.header=dc_wire_make(type,77,0,0,text?strlen(text):0);if(text)memcpy(p.payload,text,p.header.length);return p;}
static int ack(dc_phone_gate *g,const char *status,int replay){char data[512];snprintf(data,sizeof data,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"77\",\"command_id\":\"%s\",\"status\":\"%s\",\"code\":\"accepted\",\"replayed\":%s}",g->command_id,status,replay?"true":"false");dc_wire_packet p=packet(DC_EVT_TEXT,data);return dc_mode_phone_event(g,&p,101);}
static int cap(dc_phone_gate *g,int capable){dc_wire_packet p=packet(DC_EVT_TEXT,capable?"{\"v\":1,\"kind\":\"phone_status\",\"production_capable\":true}":"{\"v\":1,\"kind\":\"phone_status\",\"production_capable\":false}");return dc_mode_phone_event(g,&p,101);}
static int count_fd(void){int count=0;for(int i=0;i<256;i++)if(fcntl(i,F_GETFD)>=0)count++;return count;}
int main(void){
    dc_mode_request r;CHECK(!dc_mode_parse_request(DC_MODE_PHONE,phone,strlen(phone),&r));CHECK(!strcmp(r.source,"official_asr")&&!strcmp(r.script,"phone_doubao.sh")&&!strcmp(r.wake,"给豆包打电话"));
    CHECK(!dc_mode_parse_request(DC_MODE_ACTION_RESULT,action,strlen(action),&r));CHECK(!strcmp(dc_mode_action_text(&r),"操作已完成。"));strcpy(r.status,"failed");CHECK(strstr(dc_mode_action_text(&r),"未成功"));strcpy(r.status,"unknown");CHECK(strstr(dc_mode_action_text(&r),"尚未确认"));strcpy(r.status,"other");CHECK(!dc_mode_action_text(&r));
    CHECK(dc_mode_parse_request(DC_MODE_PHONE,action,strlen(action),&r)<0);CHECK(dc_mode_parse_request(DC_MODE_ACTION_RESULT,phone,strlen(phone),&r)<0);
    const char *bad[]={"{}","{\"v\":1,}","{\"v\":true}","{\"v\":1,\"v\":1}","{\"v\":1,\"\\u0076\":1}","{\"v\":1}junk","{\"v\":1,\"x\":\"\\uD800\"}","{\"v\":1,\"x\":\"\\u0000\"}","{\"v\":1,\"x\":\"\xc0\x80\"}"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)CHECK(dc_mode_parse_request(DC_MODE_PHONE,bad[i],strlen(bad[i]),&r)<0);
    char changed[1024];strcpy(changed,phone);char *at=strstr(changed,"phone_doubao.sh");memcpy(at,"../bad_file.sh",14);CHECK(dc_mode_parse_request(DC_MODE_PHONE,changed,strlen(changed),&r)<0);
    strcpy(changed,phone);at=strstr(changed,"call.start");memcpy(at,"shell.exec",10);CHECK(dc_mode_parse_request(DC_MODE_PHONE,changed,strlen(changed),&r)<0);
    strcpy(changed,phone);at=strstr(changed,"official_asr");memcpy(at,"untrusted_in",12);CHECK(dc_mode_parse_request(DC_MODE_PHONE,changed,strlen(changed),&r)<0);
    dc_phone_gate g=gate();dc_wire_packet ready=packet(DC_EVT_READY,NULL);CHECK(dc_mode_phone_event(&g,&ready,101)<0);
    g=gate();CHECK(!ack(&g,"completed",0));CHECK(dc_mode_phone_event(&g,&ready,102)<0);
    g=gate();CHECK(!cap(&g,1));CHECK(dc_mode_phone_event(&g,&ready,102)<0);
    g=gate();CHECK(!ack(&g,"started",0));CHECK(!cap(&g,1));CHECK(!ack(&g,"completed",0));CHECK(dc_mode_phone_event(&g,&ready,102)==1); /* Only seam, never main permission. */
    g=gate();CHECK(cap(&g,0)<0);CHECK(g.terminal);CHECK(dc_mode_phone_event(&g,&ready,102)<0);
    g=gate();CHECK(!ack(&g,"completed",1));CHECK(g.replayed&&!g.ack_state&&!g.terminal);CHECK(!ack(&g,"completed",0));CHECK(!cap(&g,1));CHECK(dc_mode_phone_event(&g,&ready,102)<0);
    g=gate();CHECK(!ack(&g,"completed",0));CHECK(!ack(&g,"completed",1));CHECK(!g.replayed);CHECK(!cap(&g,1));CHECK(dc_mode_phone_event(&g,&ready,102)==1);
    g=gate();CHECK(ack(&g,"failed",0)<0);g=gate();CHECK(ack(&g,"cancelled",0)<0);
    g=gate();CHECK(!ack(&g,"completed",0));CHECK(ack(&g,"started",0)<0);
    g=gate();CHECK(!ack(&g,"completed",0));CHECK(!cap(&g,1));ready.header.flags=DC_WIRE_FINAL;CHECK(dc_mode_phone_event(&g,&ready,102)<0);ready.header.flags=0;
    g=gate();ready.header.epoch=76;CHECK(dc_mode_phone_event(&g,&ready,102)<0);ready.header.epoch=77;
    g=gate();ready.header.generation=1;CHECK(dc_mode_phone_event(&g,&ready,102)<0);ready.header.generation=0;
    g=gate();CHECK(!ack(&g,"completed",0));CHECK(ack(&g,"started",1)<0);
    g=gate();CHECK(dc_mode_phone_tick(&g,10100)<0);CHECK(dc_mode_phone_event(&g,&ready,10101)<0);
    g=gate();dc_wire_packet arbitrary=packet(DC_EVT_TEXT,"{\"v\":1,\"kind\":\"assistant\",\"text\":\"never speak metadata\"}");CHECK(dc_mode_phone_event(&g,&arbitrary,101)<0);
    int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets));CHECK(send(sockets[0],phone,strlen(phone),0)==(ssize_t)strlen(phone));CHECK(!dc_mode_read_request(sockets[1],DC_MODE_PHONE,&r));CHECK(dc_mode_read_request(sockets[1],DC_MODE_PHONE,&r)<0);
    int sink=open("/dev/null",O_RDWR|O_CLOEXEC);CHECK(sink>=3);int before=count_fd();
    struct iovec vector={(void *)phone,strlen(phone)};union{struct cmsghdr align;char bytes[CMSG_SPACE(sizeof(int))];} control;struct msghdr m={0};m.msg_iov=&vector;m.msg_iovlen=1;m.msg_control=control.bytes;m.msg_controllen=sizeof control.bytes;struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&sink,sizeof sink);CHECK(sendmsg(sockets[0],&m,0)>0);CHECK(dc_mode_read_request(sockets[1],DC_MODE_PHONE,&r)<0);CHECK(count_fd()==before);
    CHECK(dc_mode_read_request(sink,DC_MODE_PHONE,&r)<0);close(sockets[0]);close(sockets[1]);CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));CHECK(dc_mode_read_request(sockets[1],DC_MODE_PHONE,&r)<0);close(sockets[0]);close(sockets[1]);
    CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,sockets));int extra=dup(sink);CHECK(extra>=3);pid_t child=fork();CHECK(child>=0);if(!child){int passed=!dc_mode_child_isolate(sockets[1],3,sink,128)&&fcntl(3,F_GETFD)==0&&fcntl(extra,F_GETFD)<0&&errno==EBADF;char result=(char)passed;(void)write(3,&result,1);_exit(passed?0:1);}close(sockets[1]);char passed=0;CHECK(read(sockets[0],&passed,1)==1&&passed);int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));close(sockets[0]);close(extra);close(sink);
    CHECK(dc_mode_child_limit()>=4);CHECK(dc_mode_child_isolate(-1,3,0,128)<0);
    printf("{\"passed\":true,\"assertions\":%u,\"phone_media_enabled\":false,\"actual_audio\":false}\n",checks);return 0;
}
