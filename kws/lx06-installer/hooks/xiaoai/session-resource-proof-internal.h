#ifndef LX06_SESSION_RESOURCE_PROOF_INTERNAL_H
#define LX06_SESSION_RESOURCE_PROOF_INTERNAL_H
#include "session-owner.h"
#include "session-resource-proof.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/prctl.h>
#define LX06_RESOURCE_MAGIC UINT32_C(0x31525053)
#define LX06_RESOURCE_START 1u
#define LX06_RESOURCE_CLOSED 2u
typedef struct {
    uint32_t magic;uint16_t version,kind;
    struct lx06_session_record record;
    uint64_t device,inode;
    uint32_t uid,gid,mode,links;
    char name[LX06_SESSION_NAME_SIZE];uint32_t reserved[4];
} lx06_resource_message;
_Static_assert(sizeof(lx06_resource_message)==152,"fixed local proof message");
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __fxstat64(int,int,struct stat64 *);
#define LX06_RESOURCE_FSTAT(fd,s) __fxstat64(3,(fd),(s))
#else
#define LX06_RESOURCE_FSTAT(fd,s) fstat((fd),(s))
#endif
static inline int lx06_resource_socket(int fd,LX06_SESSION_STATBUF *st) {
    int type=0;socklen_t n=sizeof(type);struct sockaddr_un a;socklen_t z=sizeof(a);
    return fd>=3&&!LX06_RESOURCE_FSTAT(fd,st)&&S_ISSOCK(st->st_mode)&&
        !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n)&&n==sizeof(type)&&type==SOCK_SEQPACKET&&
        !getsockname(fd,(struct sockaddr *)&a,&z)&&z==sizeof(sa_family_t)&&a.sun_family==AF_UNIX;
}
#endif
