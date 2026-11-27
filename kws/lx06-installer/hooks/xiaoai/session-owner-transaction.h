/* Standalone seam, NOT wired to the wake dispatcher.
 * Acquire the same nonblocking flock inode as session-run, check any recorded
 * surviving group, then invoke the callback while retaining the lease. The
 * callback must finish its synchronous side effects before returning; 1 means
 * accepted, anything else rejects with ECANCELED. This does not reserve an
 * ongoing official session after return, and is not an admission solution by
 * itself. The lock path and ancestors must remain trusted and the inode must
 * never be unlinked or replaced while any participant is running.
 *
 * Callbacks must not reenter this API, unlock/close its inherited descriptors,
 * or await a session-run process (it will be rejected while the lock is held).
 * A forked child inherits the open description until exec/close; explicit unlock
 * releases it on normal return, but a parent crash before unlock can leave a
 * child holding the lease. A successful callback followed by an unlock failure
 * has an uncertain outcome; reconcile authoritative state before retrying.
 */
#ifndef LX06_SESSION_OWNER_TRANSACTION_H
#define LX06_SESSION_OWNER_TRANSACTION_H
typedef int (*lx06_session_transaction_fn)(void *context);
int lx06_session_transaction(lx06_session_transaction_fn callback, void *context);
#endif
