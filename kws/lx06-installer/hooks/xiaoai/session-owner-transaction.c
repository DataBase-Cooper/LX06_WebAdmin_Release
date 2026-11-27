#define _GNU_SOURCE
#include "session-owner-transaction.h"
#include "session-owner.h"

int lx06_session_transaction(lx06_session_transaction_fn callback, void *context) {
    int fd, result=-1, saved;
    if(!callback) { errno=EINVAL; return -1; }
    fd=lx06_session_take_idle();
    if(fd<0) return -1;
    if(callback(context)==1) result=0;
    else errno=ECANCELED;
    saved=errno;
    int unlock_result;
    do { unlock_result=flock(fd,LOCK_UN); } while(unlock_result<0 && errno==EINTR);
    if(unlock_result && !result) { saved=errno; result=-1; }
    close(fd); errno=saved;
    return result;
}
