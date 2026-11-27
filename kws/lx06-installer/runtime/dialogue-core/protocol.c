#define _GNU_SOURCE
#include "protocol.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Local dialogue ABI currently requires little endian
#endif
static int valid(const dc_wire_header *h) {
    int known = (h->type >= DC_CMD_OPEN && h->type <= DC_CMD_GRANT) || h->type==DC_CMD_GREET ||
                (h->type >= DC_EVT_READY && h->type <= DC_EVT_RESUMED);
    if (h->magic != DC_WIRE_MAGIC || h->version != DC_WIRE_VERSION ||
        h->length > DC_WIRE_CAP || h->reserved || !known ||
        (h->flags & ~(DC_WIRE_FINAL | DC_WIRE_PCM_S16LE | DC_WIRE_RETRYABLE)))
        return 0;
    if (h->type == DC_EVT_AUDIO || h->type == DC_CMD_MIC) {
        if (!(h->flags & DC_WIRE_PCM_S16LE) ||
            h->rate < 8000 || h->rate > 48000 ||
            (h->channels != 1 && h->channels != 2) ||
            !h->length || h->length % (2u * h->channels)) return 0;
    } else if (h->rate || h->channels) return 0;
    return 1;
}
dc_wire_header dc_wire_make(unsigned type, uint64_t epoch, uint64_t generation,
                            uint64_t sequence, size_t length) {
    dc_wire_header h;
    memset(&h, 0, sizeof h);
    h.magic = DC_WIRE_MAGIC; h.version = DC_WIRE_VERSION;
    h.type = (uint16_t)type; h.epoch = epoch; h.generation = generation;
    h.sequence = sequence;
    h.length = length <= UINT32_MAX ? (uint32_t)length : UINT32_MAX;
    return h;
}
int dc_wire_send(int fd, const dc_wire_header *h, const void *payload) {
    if (!h || !valid(h) || (h->length && !payload)) { errno=EINVAL; return -1; }
    struct iovec vectors[2]={{(void *)h,sizeof *h},{(void *)payload,h->length}};
    struct msghdr message;
    memset(&message,0,sizeof message); message.msg_iov=vectors;
    message.msg_iovlen=h->length?2:1;
    ssize_t n=sendmsg(fd,&message,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n<0) return -1;
    if((size_t)n!=sizeof *h+h->length) { errno=EIO; return -1; }
    return 0;
}
int dc_wire_receive(int fd, dc_wire_packet *packet) {
    if(!packet) { errno=EINVAL; return -1; }
    struct iovec vector={packet,sizeof *packet};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(16*sizeof(int))]; } control;
    struct msghdr message;
    memset(&message,0,sizeof message);message.msg_iov=&vector;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof control.bytes;
    ssize_t n=recvmsg(fd,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(n<=0) return n==0?0:-1;
    int ancillary=0;
    for(struct cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
        ancillary=1;
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
            size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<count;i++) {int passed;memcpy(&passed,(unsigned char *)CMSG_DATA(c)+i*sizeof(int),sizeof passed);close(passed);}
        }
    }
    if(ancillary || (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) ||
       (size_t)n<sizeof packet->header || !valid(&packet->header) ||
       (size_t)n!=sizeof packet->header+packet->header.length) { errno=EPROTO;return -1; }
    return 1;
}
