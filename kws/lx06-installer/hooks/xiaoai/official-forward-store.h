/* Independent configuration seam, not connected to wake dispatch or UI.
 * Pass an already-open, private (owner=euid, mode 0700) directory fd. The
 * caller is responsible for safely opening every ancestor, provisioning it,
 * and supplying an authoritative registry lookup. Never pass a writable-by-
 * others directory. Lookup returns exactly 1 only for installed AND enabled.
 * The legacy read/resolve APIs do not reserve a target. For new callers use
 * snapshot/reserve below and separately revalidate the registry at commit.
 * read/select: 0 success, -1 failure. Missing file means official. A select
 * error after rename (e.g. directory fsync or unlock failure) leaves outcome
 * uncertain; read back and reconcile rather than assuming no change.
 */
#ifndef LX06_OFFICIAL_FORWARD_STORE_H
#define LX06_OFFICIAL_FORWARD_STORE_H
#include "official-forward-policy.h"
/* All writers MUST use of_store_select and the same securely provisioned directory;
 * direct writes, unlink/rename, chmod, or directory substitution invalidate this
 * protocol. The directory and every ancestor must be trusted throughout. Advisory
 * directory flock serializes independent processes AND threads; unsupported locking
 * fails closed. This does not make an untrusted directory safe.
 *
 * A successful snapshot pins the selected file inode (including across ABA); absent
 * file is the default official revision. Keep it alive, unmodified and process-local
 * until close; do not copy, forge, close its fds or use it after fork. Snapshot and
 * reserve return 0 on success, -1 on failure; ESTALE means selection changed.
 *
 * Reserve compares under the same lock as select and invokes commit exactly once
 * only if current. Commit must synchronously and atomically reserve the session in
 * its own authoritative shared state and return exactly 1 for success. It must
 * separately revalidate installed/enabled registry state under that registry's own
 * synchronization, and prevent subsequent unsafe target substitution. A callback
 * rejection returns -1/ECANCELED; it must leave no reservation. No reentrant store
 * calls, blocking I/O or asynchronous admission inside either callback. Lock order
 * is store -> registry -> session state; all participants must obey it and must
 * never enter the store while holding registry or session locks, nor acquire the
 * registry while holding session state. Registry lookup during select is also
 * called with the store lock held. On successful
 * return the selection was current at commit, not guaranteed to remain selected
 * afterwards. A process crash, lost response, or unlock failure after the callback
 * may leave the reservation committed despite no success response. Use an
 * idempotency key and authoritative reconciliation before retrying; callbacks are
 * invoked at most once per reserve invocation, not exactly once across retries.
 * Close failures do not revoke an already committed reservation.
 *
 * Unlock is explicit before close so a cooperative callback's forked child does
 * not retain the lock after normal return. Fork shares the locked open description:
 * a child can itself unlock it early, so callbacks and children must be trusted and
 * must not manipulate inherited lock descriptors. If the parent dies before the
 * unlock, a surviving child can keep the lock indefinitely. No crash recovery or
 * protection against a malicious child is provided by this advisory protocol.
 * No wake path is integrated by these APIs. */
struct of_store_snapshot { char selection[65]; int directory_fd; int file_fd; };
typedef int (*of_store_commit)(void *context, const char *selection);
int of_store_snapshot(int directory_fd, struct of_store_snapshot *out);
int of_store_reserve(int directory_fd, const struct of_store_snapshot *snapshot,
                     of_store_commit commit, void *context);
void of_store_snapshot_close(struct of_store_snapshot *snapshot);
typedef int (*of_store_lookup)(void *context, const char *id);
int of_store_read(int directory_fd, char out[65]);
int of_store_select(int directory_fd, const char *selection, of_store_lookup lookup, void *context);
enum of_result of_store_resolve(int directory_fd, of_store_lookup lookup, void *context, char out[65]);
#endif
