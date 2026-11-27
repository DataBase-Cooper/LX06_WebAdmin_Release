#ifndef LX06_OWNED_STORE_H
#define LX06_OWNED_STORE_H
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
/* Standalone primitive, NOT a migration or transaction implementation.
 * IDs are independently pinned by the caller, not read from the journal.
 * Use unique unpredictable 128-bit store and transaction IDs; retain them in
 * trusted durable state, never reuse a transaction ID, and establish an
 * exclusive namespace boundary. The primitive cannot enforce those duties.
 * No authentication against a writer able to forge records or IDs, no rollback
 * detection (including complete journal removal), no guarantee against copying
 * the whole trusted state/store. Reusing IDs or accepting them from the store
 * defeats the binding; uniqueness cannot be enforced here. V1 records are
 * refused, not upgraded. No production caller or recovery protocol exists. */
#define OWNED_STORE_ID_SIZE 16
struct owned_store { int dirfd, lockfd; dev_t dev; ino_t dirino, lockino; uid_t owner; pid_t opener_pid; unsigned char store_id[OWNED_STORE_ID_SIZE], txn_id[OWNED_STORE_ID_SIZE]; };
typedef void (*owned_store_checkpoint)(const char *, void *);
int owned_store_open(struct owned_store *, int, dev_t, ino_t, const unsigned char store_id[OWNED_STORE_ID_SIZE], const unsigned char txn_id[OWNED_STORE_ID_SIZE]);
int owned_store_phase(struct owned_store *, int *);
int owned_store_append(struct owned_store *, int, owned_store_checkpoint, void *);
/* Fixed canonical wire image, in memory only. Entries are model, recordings,
 * config in that order. All six names are distinct single components inside
 * one caller-defined namespace; dev/inode are claimed identities, not stat
 * checks. CRC detects accidental damage, not malicious forgery. No persistence,
 * exclusive publication, immutable storage, authentication, rollback or
 * crash recovery is implemented. Never use this as authorization to mutate. */
#define OWNED_MANIFEST_NAME_SIZE 64
#define OWNED_MANIFEST_SIZE 549
#define OWNED_MANIFEST_RENAME 1
struct owned_manifest_entry {
    char old_name[OWNED_MANIFEST_NAME_SIZE], new_name[OWNED_MANIFEST_NAME_SIZE];
    uint64_t old_dev, old_ino, new_dev, new_ino;
};
struct owned_store_manifest { unsigned int operation; struct owned_manifest_entry entry[3]; };
int owned_store_manifest_encode(struct owned_store *, const struct owned_store_manifest *, unsigned char *, size_t);
int owned_store_manifest_validate(struct owned_store *, const unsigned char *, size_t, struct owned_store_manifest *);
/* All six v1 objects must exist; entry 0 is directories, 1/2 regular
 * files with link count one. Returned O_PATH descriptors pin identity only;
 * caller must exclude namespace writers and recheck immediately before use.
 * No mutation, transaction, recovery, authentication, or inode-generation
 * guarantees are provided. O_TMPFILE/linkat support is required for bind. */
struct owned_store_pins { int fd[6]; };
int owned_store_pin_manifest(struct owned_store *, const unsigned char *, size_t, struct owned_store_pins *);
void owned_store_unpin(struct owned_store_pins *);
int owned_store_bind(struct owned_store *, const unsigned char *, size_t);
int owned_store_bound_phase(struct owned_store *, const unsigned char *, size_t, int *);
void owned_store_close(struct owned_store *);
#endif
