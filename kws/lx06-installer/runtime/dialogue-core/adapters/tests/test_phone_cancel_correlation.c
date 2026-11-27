/* Offline real command receiver, WebSocket encoder, dispatcher and local sender.
 * This checks ACK identity only; it does not establish a real turn fence. */
#define _GNU_SOURCE
#ifdef NDEBUG
#error assertions required
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "../phone.c"

int main(void) {
    int sv[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_NONBLOCK, 0, sv)==0);
    struct phone *w=calloc(1,sizeof *w); assert(w);
    w->core=sv[0]; w->socket=-1; w->opened=1; w->ready=1;
    w->epoch=123; w->generation=17; w->reply=9;
    dc_wire_packet ack={0};
    ack.header=dc_wire_make(DC_EVT_CANCEL_ACK,123,17,9,0);
    w->frozen=1; /* Freeze alone is not proof that a CANCEL was sent. */
    assert(remote_packet(w,(unsigned char *)&ack,sizeof ack.header)<0);
    assert(w->local.count==0); w->frozen=0;
    dc_wire_header cancel=dc_wire_make(DC_CMD_CANCEL,123,17,9,0);
    assert(dc_wire_send(sv[1],&cancel,NULL)==0);
    assert(commands(w)==0 && w->frozen && w->cancel_sent && w->net.count==1);
    unsigned char decoded[WCAP]; dc_xz_ws_frame frame; size_t used=0;
    assert(dc_xz_ws_decode_frame(w->net.f[0].b,w->net.f[0].n,1,&frame,decoded,sizeof decoded,&used)==0);
    assert(used==sizeof cancel && !memcmp(decoded,&cancel,sizeof cancel));
    ack.header=dc_wire_make(DC_EVT_CANCEL_ACK,123,17,8,0); /* previous turn */
    assert(remote_packet(w,(unsigned char *)&ack,sizeof ack.header)<0);
    assert(w->local.count==0);
    ack.header.sequence=9;
    assert(remote_packet(w,(unsigned char *)&ack,sizeof ack.header)==0);
    assert(local_flush(w)==0);
    dc_wire_packet received;
    assert(dc_wire_receive(sv[1],&received)==1);
    assert(received.header.type==DC_EVT_CANCEL_ACK && received.header.sequence==9);
    close(sv[0]); close(sv[1]); free(w);
    puts("cancel ACK sequence correlation: PASS");
    return 0;
}
