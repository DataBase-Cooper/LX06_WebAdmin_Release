#define _GNU_SOURCE
#include "../control.h"
#include "../protocol.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef NDEBUG
#error assertions required
#endif

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while(0)
static dc_config config(void) {
    dc_config c={.idle_ms=10000,.tail_ms=300};
    strcpy(c.interrupt,"等一下,等等,你说得不对,停一下");
    strcpy(c.goodbye,"结束对话,拜拜了哦");return c;
}
static int fd_count(void){DIR*d=opendir("/proc/self/fd");assert(d);int n=0;while(readdir(d))n++;closedir(d);return n;}
int main(void) {
    dc_control c;dc_config conf=config();dc_decision d;dc_tag source;
    CHECK(dc_control_open(&c,71,&conf,100)==0);
    CHECK(c.state==DC_CONNECTING&&!c.closed);
    source=(dc_tag){71,0,1};
    CHECK(dc_control_text(&c,source,"请用两句话",0,0,200,&d)==0);
    CHECK(!(d.actions&DC_ACT_QUESTION));
    CHECK(dc_control_text(&c,source,"请用两句话介绍北京故宫。",1,0,300,&d)==1);
    CHECK(!strcmp(d.question,"请用两句话介绍北京故宫"));
    dc_tag reply=dc_control_reply(&c,1,400);
    CHECK(reply.reply_generation==1&&dc_control_state(&c,reply,DC_SPEAKING,450)==0);
    source=(dc_tag){71,1,2};
    CHECK(dc_control_text(&c,source,"等一下",0,1,500,&d)==0);
    CHECK(c.reply_live);
    CHECK(dc_control_text(&c,source,"普通环境说话",0,0,510,&d)==0&&c.reply_live);
    CHECK(dc_control_text(&c,source,"等一下",0,0,520,&d)==1);
    CHECK((d.actions&(DC_ACT_STOP|DC_ACT_CANCEL))==(DC_ACT_STOP|DC_ACT_CANCEL));
    CHECK(!(d.actions&DC_ACT_END)&&!c.closed&&c.state==DC_LISTENING);
    CHECK(dc_control_text(&c,source,"等一下，等一下，你说得不对，停一下",0,0,530,&d)==0);
    CHECK(dc_control_text(&c,source,"等一下，等一下，你说得不对。一加一等于几，只回答数字。",1,0,540,&d)==1);
    CHECK(d.actions==DC_ACT_QUESTION);
    CHECK(!strcmp(d.question,"一加一等于几，只回答数字"));
    CHECK(dc_control_text(&c,source,"等一下，一加一等于几",1,0,550,&d)==0);
    dc_tag next=dc_control_reply(&c,2,600);CHECK(next.reply_generation==2);
    CHECK(dc_control_text(&c,source,"等一下，旧的迟到结果",1,0,610,&d)==0);
    CHECK(c.reply_live&&c.tag.reply_generation==2);
    /* A replay rebound to the active generation must not match controls. */
    source=(dc_tag){71,next.reply_generation,2};
    CHECK(dc_control_text(&c,source,"等一下",0,0,611,&d)==0);
    CHECK(!d.interrupt_matched&&!d.actions&&c.reply_live&&!c.closed);
    CHECK(dc_control_text(&c,source,"结束对话",1,0,612,&d)==0);
    CHECK(!d.interrupt_matched&&!d.actions&&c.reply_live&&!c.closed);
    CHECK(dc_control_finish_reply(&c,reply,620)==-1);
    CHECK(dc_control_finish_reply(&c,next,630)==0&&c.state==DC_LISTENING&&!c.closed);
    CHECK(dc_control_tick(&c,1000,&d)==0);
    CHECK(dc_control_tick(&c,10631,&d)==1&&(d.actions&DC_ACT_END));
    CHECK(c.state==DC_CLOSING);
    CHECK(dc_control_close(&c,10632,&d)==0);
    CHECK(dc_control_finish_close(&c,10633,&d)==1&&c.state==DC_CLOSED);
    CHECK(dc_control_finish_close(&c,10634,&d)==0);

    conf.barge_any=1;CHECK(dc_control_open(&c,72,&conf,100)==0);
    reply=dc_control_reply(&c,1,200);source=(dc_tag){72,reply.reply_generation,2};
    CHECK(dc_control_voice(&c,source,1,1,250,&d)==0&&c.reply_live);
    CHECK(dc_control_voice(&c,source,1,0,260,&d)==1&&!c.closed);
    CHECK(dc_control_voice(&c,source,1,0,270,&d)==0);
    next=dc_control_reply(&c,2,280);source=(dc_tag){72,next.reply_generation,3};
    CHECK(dc_control_voice(&c,source,1,0,300,&d)==0); /* residual output tail */
    CHECK(dc_control_text(&c,source,"结束对话，等一下，拜拜了哦",0,0,310,&d)==1);
    CHECK((d.actions&DC_ACT_END)&&!(d.actions&DC_ACT_QUESTION)&&c.closed);
    CHECK(dc_control_text(&c,source,"结束对话，拜拜了哦",1,0,320,&d)==0);
    char stripped[2048];
    CHECK(dc_control_strip(stripped,sizeof stripped,"等 一 下，等等，请讲故宫。",conf.interrupt)==0);
    CHECK(!strcmp(stripped,"请讲故宫"));
    CHECK(dc_control_strip(stripped,sizeof stripped,"等一下，等等，停一下",conf.interrupt)==0&&!stripped[0]);
    CHECK(dc_control_strip(stripped,sizeof stripped,"\xff",conf.interrupt)==-1);

    int sockets[2];CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0,sockets)==0);
    dc_wire_header h=dc_wire_make(DC_EVT_TEXT,72,2,11,6);dc_wire_packet p;
    CHECK(dc_wire_send(sockets[0],&h,"hello!")==0);
    CHECK(dc_wire_receive(sockets[1],&p)==1&&p.header.sequence==11&&!memcmp(p.payload,"hello!",6));
    CHECK(dc_wire_receive(sockets[1],&p)==-1&&errno==EAGAIN);
    h.reserved=1;CHECK(dc_wire_send(sockets[0],&h,"hello!")==-1&&errno==EINVAL);
    CHECK(send(sockets[0],"bad",3,0)==3);
    CHECK(dc_wire_receive(sockets[1],&p)==-1&&errno==EPROTO);
    h=dc_wire_make(DC_EVT_TEXT,72,2,11,0);
    char ancillary[CMSG_SPACE(sizeof(int))]={0};struct iovec vector={&h,sizeof h};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=ancillary,.msg_controllen=sizeof ancillary};
    struct cmsghdr *cm=CMSG_FIRSTHDR(&message);cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm),&sockets[0],sizeof(int));int before=fd_count();
    CHECK(sendmsg(sockets[0],&message,0)==sizeof h);
    CHECK(dc_wire_receive(sockets[1],&p)==-1&&errno==EPROTO);
    CHECK(fd_count()==before);
    close(sockets[0]);CHECK(dc_wire_receive(sockets[1],&p)==0);close(sockets[1]);
    printf("{\"passed\":true,\"checks\":%u,\"device_access\":false}\n",checks);
    return 0;
}
