/* LX06 dialogue light API v1. Best effort, nonblocking, session-local only.
 * Keep this header usable by both firmware and independently built clients. */
#ifndef LX06_DIALOG_LIGHT_H
#define LX06_DIALOG_LIGHT_H
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

enum lx06_light_state {
    LX06_LIGHT_END, LX06_LIGHT_WAKE, LX06_LIGHT_LISTENING,
    LX06_LIGHT_THINKING, LX06_LIGHT_SPEAKING, LX06_LIGHT_INTERRUPTED,
    LX06_LIGHT_COUNT
};
static inline const char *lx06_light_name(int state) {
    static const char *const names[]={"ending","wake","listening","thinking","speaking","interrupted"};
    return state>=0 && state<LX06_LIGHT_COUNT ? names[state] : "invalid";
}
static inline int lx06_dialog_light(enum lx06_light_state state) {
    const char *value=getenv("LX06_DIALOG_FD"), *identity=getenv("LX06_DIALOG_CHANNEL");
    if(!value || !identity || state<0 || state>=LX06_LIGHT_COUNT) return -1;
    char *end, path[64], actual[96];
    long fd=strtol(value,&end,10);
    if(*end || fd<3 || fd>1024) return -1;
    snprintf(path,sizeof(path),"/proc/self/fd/%ld",fd);
    ssize_t n=readlink(path,actual,sizeof(actual)-1);
    if(n<=0) return -1;
    actual[n]=0;
    /* A reused fd must never turn this API into a write to an audio/network fd. */
    if(strncmp(actual,"socket:[",8) || strcmp(actual,identity)) return -1;
    unsigned char message=(unsigned char)state;
    return send((int)fd,&message,1,MSG_DONTWAIT|MSG_NOSIGNAL)==1 ? 0 : -1;
}
#endif
