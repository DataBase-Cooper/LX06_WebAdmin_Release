/* Offline experiment only: not integrated into session-run or dialogue core. */
#ifndef LX06_STATUS_PIPE_EXPERIMENT_H
#define LX06_STATUS_PIPE_EXPERIMENT_H
#include <stddef.h>
#include <stdint.h>
enum { SP_NONE=0, SP_CAPTURE_OVERFLOW=1, SP_TOKEN_BYTES=16, SP_MESSAGE_BYTES=20 };
struct sp_sender { int fd; unsigned char token[SP_TOKEN_BYTES]; };
struct sp_owner { int fd; int active; unsigned latest; unsigned char token[SP_TOKEN_BYTES]; unsigned char pending[SP_MESSAGE_BYTES]; size_t used; };
int sp_begin(struct sp_owner *, struct sp_sender *);
void sp_child_close(struct sp_owner *);
void sp_parent_close_sender(struct sp_sender *);
int sp_send(const struct sp_sender *, unsigned);
int sp_drain(struct sp_owner *);
int sp_end(struct sp_owner *);
#endif
