/* Public synthetic workload only. Both versions use the same compiler/options,
 * target and conservative nice/iteration/time budget. Not a production mode. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <sys/time.h>
#include "../hash23.h"
#include "hash23-reference.h"

static double milliseconds(clockid_t kind) {
    struct timespec value;if(clock_gettime(kind,&value))return -1;
    return value.tv_sec*1000.0+value.tv_nsec/1000000.0;
}
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    char *tail;long answer=strtol(argv[2],&tail,10);if(*tail||answer<0||answer>=200000)return 2;
    long prefix=strtol(argv[3],&tail,10);if(*tail||prefix<15||prefix>200)return 2;
    if(strcmp(argv[1],"old")&&strcmp(argv[1],"new"))return 2;
    char salt[201],message[256];memset(salt,'B',(size_t)prefix-15);salt[prefix-15]=0;
    int n=snprintf(message,sizeof(message),"%s_1700000000000_%ld",salt,answer);
    unsigned char target[32];reference_hash23_digest((unsigned char *)message,(size_t)n,target);
    double wall=milliseconds(CLOCK_MONOTONIC),cpu=milliseconds(CLOCK_PROCESS_CPUTIME_ID);
    int found=!strcmp(argv[1],"old")?reference_hash23_solve(salt,"1700000000000",(unsigned)answer+1,target):
                                      hash23_solve(salt,"1700000000000",(unsigned)answer+1,target);
    cpu=milliseconds(CLOCK_PROCESS_CPUTIME_ID)-cpu;wall=milliseconds(CLOCK_MONOTONIC)-wall;
    printf("{\"variant\":\"%s\",\"answer\":%d,\"iterations\":%ld,\"prefix_bytes\":%ld,\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"nice\":%d}\n",
           argv[1],found,answer+1,prefix,wall,cpu,getpriority(PRIO_PROCESS,0));
    return found==answer?0:3;
}
