#ifndef LX06_UNIFIED_H
#define LX06_UNIFIED_H
void lx06_unified_close_inherited(void);
int  lx06_unified_instance_acquire(void);
void lx06_unified_close_lock_fd(void);
void lx06_unified_kill_stale_webd(void);
int  lx06_unified_http_start(void);
int  lx06_unified_http_pid(void);
void lx06_unified_http_stop(void);
void lx06_unified_http_watchdog(void);
#endif
