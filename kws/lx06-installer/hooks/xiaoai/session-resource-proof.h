/* Explicit local direct-child channel. This observes the original owner-issued
 * controlled-descendant reap, own original lease FD close and local same-inode
 * lock-release observation; it is not global idle, a source permission,
 * durable K6 receipt or proof the supervisor itself was externally waited.
 * Caller owns the original forked supervisor, keeps the endpoint private and
 * drains START before externally reaping it. Kernel credentials bind this
 * existing trusted sender domain, not a defence against arbitrary root code. */
#ifndef LX06_SESSION_RESOURCE_PROOF_H
#define LX06_SESSION_RESOURCE_PROOF_H
#include <sys/types.h>
struct lx06_session_record;
typedef struct lx06_session_resource_proof lx06_session_resource_proof;
/* Fresh unnamed nonblocking SEQPACKET, CLOEXEC on both ends; pass only end1
 * through the direct child's exec as LX06_SESSION_PROOF_FD, then close end1 in
 * the parent. Session-run immediately restores CLOEXEC and hides it from its
 * business child. End0 has kernel sender credentials enabled before launch. */
int lx06_session_resource_channel(int pair[2]);
/* Takes owned end0 only on success. expected is the caller's actual fork PID,
 * never a PID adopted from a status/name lookup. Name is the original launch
 * argument. Fresh state only; do not reuse a completed handle for another job. */
lx06_session_resource_proof *lx06_session_resource_open(int end0,pid_t expected,const char *name);
/* Sole serial owner, off audio thread: 0 pending, 1 authenticated original
 * controlled-descendant reap, own FD close and local same-inode lock-release
 * observation, -1 latched unknown/refusal. No signals, lock
 * acquisition, filesystem writes, waitpid, replacement adoption or retry. */
int lx06_session_resource_poll(lx06_session_resource_proof *);
int lx06_session_resource_identity(const lx06_session_resource_proof *,struct lx06_session_record *);
/* Closes only the owned observer endpoint. Never kills/reaps or releases a
 * source; caller still owns cleanup/UNKNOWN and the supervisor's actual wait. */
void lx06_session_resource_free(lx06_session_resource_proof *);
#endif
