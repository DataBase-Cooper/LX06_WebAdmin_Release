/* Native-TTS service loan or explicit session-private broker lifecycle. No SDK/auth/snapshot processing
 * occurs here: the reviewed broker alone handles RAM snapshot IPC. */
#ifndef LX06_DC_BOOTSTRAP_H
#define LX06_DC_BOOTSTRAP_H
#include <stdint.h>
#include <sys/types.h>
typedef struct dc_bootstrap dc_bootstrap;
typedef struct dc_bootstrap_config {
 const char *external_socket; /* or LX06_NATIVE_TTS_SOCKET; default supervised boot-service socket; never start/stop/delete it */
 pid_t external_peer_pid; /* optional exact pin, uid0 always required */
 const char *private_base; /* explicit nonempty path opts into original managed mode; new lx06-dialogue.<epoch> directory */
 const char *broker_path,*worker_path,*snapshot_client_path;
 const char *sdk_path; /* mandatory managed input or LX06_NATIVE_TTS_SDK */
 pid_t mico_pid; /* >1 explicit; otherwise require mico_comm (or env) unique */
 const char *mico_comm,*proc_root; /* proc_root defaults /proc, scanner only */
} dc_bootstrap_config;
enum dc_bootstrap_state { DC_BOOT_STARTING=1,DC_BOOT_READY,DC_BOOT_STOPPING,DC_BOOT_STOPPED };
enum dc_bootstrap_error { DC_BOOT_OK=0,DC_BOOT_ERR_SPAWN,DC_BOOT_ERR_PEER,
 DC_BOOT_ERR_DEADLINE,DC_BOOT_ERR_PROCESS,DC_BOOT_ERR_CLEANUP,DC_BOOT_ERR_LIMIT };
/* Normal callers borrow /tmp/run/lx06-native-audio-state/tts.sock and retain
 * the existing external socket/peer/birth checks. No service process or socket
 * is reclaimed by this borrower. Explicit managed mode requires root and PR_SET_CHILD_SUBREAPER=1 on the calling core
 * before create. No process-global role is silently changed by this module. */
dc_bootstrap *dc_bootstrap_create(uint64_t epoch,const dc_bootstrap_config *,uint64_t now_ms);
/* Tick never waits for a child. 1=listener identity verified,0=pending/closed,
 * -1=recorded failure. READY is listener identity, never SDK/cloud readiness. */
int dc_bootstrap_tick(dc_bootstrap *,uint64_t now_ms);
void dc_bootstrap_stop(dc_bootstrap *,uint64_t now_ms);
int dc_bootstrap_quiescent(const dc_bootstrap *);
int dc_bootstrap_destroy(dc_bootstrap *); /* -1/EBUSY until quiescent */
const char *dc_bootstrap_socket(const dc_bootstrap *);
pid_t dc_bootstrap_broker_pid(const dc_bootstrap *);
enum dc_bootstrap_state dc_bootstrap_state(const dc_bootstrap *);
enum dc_bootstrap_error dc_bootstrap_error(const dc_bootstrap *);
/* Fixed-name /proc/comm scan; never logs process names, args, environment or
 * account files. Exactly one live root process is required. */
int dc_bootstrap_find_mico(const char *proc_root,const char *comm,pid_t *pid);
#endif
