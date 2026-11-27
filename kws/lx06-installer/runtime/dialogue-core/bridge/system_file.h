/* T18 local resource admission. PROVISIONAL(P-20261111-1245-B).
 * No path defaults, globbing, network, shell, writes or player. */
#ifndef LX06_BR_SYSTEM_FILE_H
#define LX06_BR_SYSTEM_FILE_H
#include <stddef.h>
#define BR_SYSTEM_PATH_MAX 255u
int br_system_path_valid(const char *);
/* root is an owner-authorized directory FD, never an untrusted path. Exact
 * allowlist, 1..32 entries. Opens each component without following symlinks.
 * Returns a NEW readonly CLOEXEC regular-file FD, or -1. Caller closes it.
 * The FD pins identity across rename; owner must keep its CONTENT immutable.
 * No real-time/audio callback may call this filesystem admission function. */
int br_system_file_open(int root,const char *path,const char *const *allowed,size_t count);
#endif
