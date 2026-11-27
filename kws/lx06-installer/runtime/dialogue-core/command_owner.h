/* Local receipt adapter for the original serial bridge session and inbox.
 * No production registration, transport/reap owner, result queue or Ledger.
 * Session, inbox and this disjoint state outlive the callback. The original
 * owner serializes all calls off the audio thread. Zero-initialize once and
 * bind the SAME original session/inbox; do not replace them in a callback. */
#ifndef LX06_COMMAND_OWNER_H
#define LX06_COMMAND_OWNER_H
#include "bridge/session.h"
#include "bridge/result_inbox.h"

typedef struct br_command_owner {
    const br_session *session;
    br_inbox *inbox;
    br_result scratch; /* identity only; erased after every attempted lookup */
    int busy;
} br_command_owner;

/* br_session_io.receipt signature. The original io owner may delegate its
 * receipt callback here with this state. Called only inside the original
 * session's nonreentrant ledger proof scope, after actual oneway GRANT.
 * Derives current run/epoch/index/generation/command/task from that session,
 * then finds the UNIQUE original origin0 row and asks the original inbox's
 * ledger_result API for its exact epoch/command/task/app/hook receipt.
 * Rechecks session/caps/ticket and original immutable row after association.
 * 1=original receipt, 0=absent, negative=refused/blocked. No enqueue, speech,
 * TTL change, retry, business terminal, UNKNOWN or resource release is added.
 * Original br_session_ledger alone adopts the returned proof. Original close
 * and actual reaped callbacks remain the transport/resource owner's duty.
 * Like ledger_result, this trusts original inbox admission/storage ownership;
 * it does not reread disk or turn a speech state into business success. */
int br_command_owner_receipt(void *,uint64_t run,uint64_t epoch,
                             const char *command,const char *task);
#endif
