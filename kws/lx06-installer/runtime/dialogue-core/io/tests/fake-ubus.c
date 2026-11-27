/* Synthetic local ledger only; never calls ubus or accesses a real LED. */
#define _GNU_SOURCE
#include "led-fixture.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
static void path(char*out,const char*name){snprintf(out,256,"%s/%s",getenv("DC_LED_DIRECTORY"),name);}
int main(int argc,char**argv){
    if(argc==6&&!strcmp(argv[3],"call")&&!strcmp(argv[4],"led")&&!strcmp(argv[5],"status")) {
        char file[256];path(file,"ledger");int fd=open(file,O_RDONLY|O_CLOEXEC);
        struct led_fixture_record record;
        if(fd<0||flock(fd,LOCK_SH)||read(fd,&record,sizeof(record))!=sizeof(record))return 3;
        close(fd);
        if(!record.bits) {puts("{\"info\":\"\"}");return 0;}
        printf("{\"info\":\"{ \\\"effect\\\": [");
        int separator=0;
        for(unsigned type=0;type<16;type++) if(record.bits&(1u<<type)) {
            printf("%s { \\\"type\\\": %u }",separator?",":"",type);separator=1;
        }
        puts(" ] }\"}");
        return 0;
    }
    if(argc!=7||strcmp(argv[3],"call")||strcmp(argv[4],"led"))return 2;
    int effect=-1;if(sscanf(argv[6],"{\"L\":%d}",&effect)!=1||effect<0||effect>15)return 2;
    int show=!strcmp(argv[5],"show");
    /* Actual owner argv is: ubus -t 1 call led show/shut JSON. */
    if(strcmp(argv[5],"show")&&strcmp(argv[5],"shut"))return 2;
    char file[256];path(file,"delay");
    if(show&&effect==3&&!access(file,F_OK)){
        path(file,"started");int marker=open(file,O_WRONLY|O_CREAT,0600);if(marker>=0)close(marker);
        struct timespec wait={0,300000000};nanosleep(&wait,NULL);
    }
    path(file,"fail");if(show&&!access(file,F_OK))return 1;
    path(file,"ledger");int fd=open(file,O_RDWR|O_CLOEXEC);if(fd<0||flock(fd,LOCK_EX))return 3;
    struct led_fixture_record record;if(pread(fd,&record,sizeof(record),0)!=sizeof(record))return 3;
    if(show){record.bits|=1u<<(unsigned)effect;if(effect==3)record.show3++;}
    else record.bits&=~(1u<<(unsigned)effect);
    record.operations++;path(file,"active");if(!record.bits&&!access(file,F_OK))record.dark_while_active++;
    int ok=pwrite(fd,&record,sizeof(record),0)==sizeof(record);close(fd);return ok?0:3;
}
