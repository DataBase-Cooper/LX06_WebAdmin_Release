/* Local, root-only command router protocol. No shell text or network listener. */
#ifndef LX06_OFFICIAL_ROUTER_PROTOCOL_H
#define LX06_OFFICIAL_ROUTER_PROTOCOL_H
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#ifndef OR_SOCKET
#define OR_SOCKET "/tmp/lx06-official-router.sock"
#endif
#define OR_MAGIC 0x4f523031U
#ifndef OR_CANCELED
#define OR_CANCELED "/tmp/lx06-official-canceled"
#endif
#define OR_MAX_CANCELED 4096
struct or_canceled { uint32_t magic,count;char ids[OR_MAX_CANCELED][96]; };
enum { OR_PLAY=1, OR_PING=2 };
struct or_query { uint32_t magic, action; char dialog[96], url[512]; };
struct or_reply { uint32_t magic; int32_t blocked; };
/* A dead router must not freeze the official player. The caller fails open;
 * the dispatcher in turn refuses to launch scripts without a live gate. */
static inline int or_call(const struct or_query *q) {
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0) return -1;
    struct sockaddr_un sa={.sun_family=AF_UNIX};
    strncpy(sa.sun_path,OR_SOCKET,sizeof(sa.sun_path)-1);
    int result=-1;
    if(connect(fd,(void *)&sa,sizeof(sa))==0) {
        struct ucred peer; socklen_t n=sizeof(peer);
        if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n) && peer.uid==0 &&
           send(fd,q,sizeof(*q),MSG_NOSIGNAL)==sizeof(*q)) {
            struct pollfd p={fd,POLLIN,0};
            struct or_reply r;
            if(poll(&p,1,120)>0 && recv(fd,&r,sizeof(r),MSG_TRUNC)==sizeof(r) && r.magic==OR_MAGIC)
                result=r.blocked;
        }
    }
    close(fd);return result;
}
#endif
