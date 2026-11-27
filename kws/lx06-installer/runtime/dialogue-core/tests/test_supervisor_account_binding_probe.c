/* Real account configuration/session and account runner under real session-run.
 * Test adapter: pauses after account configuration so the test can atomically
 * replace the synthetic upload before the first actual model request. */
#define main dialogue_production_main
#include "../main.c"
#undef main

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--protocol")){puts("LX06DIALOGUE/1");return 0;}
    if(argc<4 || strcmp(argv[1],"--account"))return 2;
    const char *sealed=getenv("DC_ACCOUNT_SEALED_FD");
    int fd=sealed?inherited_fd(sealed):-1;
    if(sealed&&fd<0)return 2;
    struct core c={0};dc_config cfg;
    if(configure_account(&c,&cfg,123456,fd))return 3;
    printf("MODE %u\n",cfg.barge_any);fflush(stdout);
    const char *gate=getenv("FIXTURE_GATE");
    if(!gate)return 4;
    uint64_t gate_until=now_ms()+3000;
    while(access(gate,F_OK) && now_ms()<gate_until)usleep(1000);
    if(access(gate,F_OK))return 4;
    dc_tag tag={123456,1,1};
    if(dc_account_start(c.account,tag,"synthetic question",argv[2],"synthetic",now_ms()))return 5;
    uint64_t until=now_ms()+3000;int done=0;
    while(now_ms()<until&&!done){
        dc_account_event event;int r=dc_account_poll(c.account,now_ms(),&event);
        if(r<0)return 6;
        if(r&&event.type==DC_ACCOUNT_DELTA){printf("DELTA %s\n",event.text);fflush(stdout);}
        if(r&&event.type==DC_ACCOUNT_DONE)done=1;
        if(r&&event.type==DC_ACCOUNT_ERROR)return 7;
        usleep(1000);
    }
    if(!done)return 8;
    if(dc_account_destroy(c.account))return 9;
    return 0;
}
