/* Experimental fixed-record anonymous pipe. No production wiring. */
#define _GNU_SOURCE
#include "status-pipe-experiment.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int sp_begin(struct sp_owner *s, struct sp_sender *sender) {
 int fds[2], random; size_t n=0; unsigned char token[SP_TOKEN_BYTES];
 if (!s || !sender || s->active) { errno=EINVAL; return -1; }
 random=open("/dev/urandom", O_RDONLY|O_CLOEXEC); if(random<0) return -1;
 while(n<sizeof token) { ssize_t got=read(random,token+n,sizeof token-n); if(got<0&&errno==EINTR) continue; if(got<=0) { int e=got==0?EIO:errno; close(random); errno=e; return -1; } n+=(size_t)got; }
 close(random);
 if(pipe2(fds,O_CLOEXEC|O_NONBLOCK)) return -1;
 s->fd=fds[0]; s->active=1; s->latest=SP_NONE; s->used=0; memcpy(s->token,token,sizeof token);
 sender->fd=fds[1]; memcpy(sender->token,token,sizeof token); return 0;
}
void sp_child_close(struct sp_owner *s) { if(s&&s->active) { close(s->fd); s->fd=-1; } }
void sp_parent_close_sender(struct sp_sender *s) { if(s&&s->fd>=0) { close(s->fd); s->fd=-1; } }
int sp_send(const struct sp_sender *s,unsigned code) {
 unsigned char b[SP_MESSAGE_BYTES]={0}; ssize_t n;
 if(!s||s->fd<0||code!=SP_CAPTURE_OVERFLOW) { errno=EINVAL; return -1; }
 memcpy(b,s->token,SP_TOKEN_BYTES); b[SP_TOKEN_BYTES]=(unsigned char)code;
 do n=write(s->fd,b,sizeof b); while(n<0&&errno==EINTR);
 if(n==(ssize_t)sizeof b) return 0;
 if(n>=0) errno=EIO;
 return -1;
}
int sp_drain(struct sp_owner *s) {
 if(!s||!s->active||s->fd<0) { errno=EINVAL; return -1; }
 for(;;) {
  ssize_t n=read(s->fd,s->pending+s->used,SP_MESSAGE_BYTES-s->used);
  if(n<0&&errno==EINTR) continue;
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) return 0;
  if(n<0) return -1;
  if(!n) { if(s->used) { errno=EPROTO; return -1; } return 0; }
  s->used+=(size_t)n;
  if(s->used==SP_MESSAGE_BYTES) {
   if(!memcmp(s->pending,s->token,SP_TOKEN_BYTES) && s->pending[16]==SP_CAPTURE_OVERFLOW && !s->pending[17] && !s->pending[18] && !s->pending[19]) s->latest=SP_CAPTURE_OVERFLOW;
   s->used=0;
  }
 }
}
int sp_end(struct sp_owner *s) { if(!s||!s->active) {errno=EINVAL;return -1;} close(s->fd); s->fd=-1; s->active=0; s->used=0; return 0; }
