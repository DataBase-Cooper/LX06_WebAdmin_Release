#ifndef LX06_WW_STORE_H
#define LX06_WW_STORE_H
#include <stddef.h>
#define WW_ACCEPTED_GATE_FIELD "p05_accepted_gate="
/* Wakeword directory transactions. Callers pass the wakeword directory only.
 * Nothing outside that directory is opened for write. A same-UID process that
 * already holds a directory fd, or that can replace a parent mount, is outside
 * what this directory-fd protocol can stop; that limit is stated in
 * docs/wakeword-owned-store-recovery-design.md and still needs a device
 * mount/user boundary. */
int ww_store_migrate(const char *dir, const char *feature);
int ww_store_replace_conf(const char *dir, const char *text, size_t n);
int ww_store_remove_word(const char *dir, const char *name);
int ww_store_mark_unready(const char *dir, const char *name);
int ww_store_publish_model(const char *dir, const char *name, const char *model_path,
                           const char *threshold, const char *feature, const char *sha);
#endif
