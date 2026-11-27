/* Offline integration of production initialization/configuration and runner.
 * Build on Linux: gcc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -ffunction-sections
 * -fdata-sections tests/test_account_mode_binding.c account.c session.c control.c ../doubao/src/text_match.c
 * -Wl,--gc-sections -lm -o /dev/shm/dc-mode && /dev/shm/dc-mode
 * Synthetic memfds only; this does not validate a trusted launcher. */
#define main dialogue_production_main
#include "../main.c"
#undef main
#include <sys/mman.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static void noop_tag(void *p,dc_tag tag){(void)p;(void)tag;}
static void noop_close(void *p){(void)p;}
static int noop_question(void *p,dc_tag tag,const char *q){(void)p;(void)tag;(void)q;return 0;}
static void noop_light(void *p,dc_tag tag,uint64_t seq,enum dc_state state){(void)p;(void)tag;(void)seq;(void)state;}

static int fixture(const char *mode) {
    int fd=memfd_create("synthetic",MFD_ALLOW_SEALING);
    if(fd<0)return -1;
    char body[512];
    int n=snprintf(body,sizeof body,"#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: %s\nprintf '%%s\\n' '{\"delta\":\"bound\"}' '{\"done\":true}'\n",mode);
    if(n<0 || write(fd,body,(size_t)n)!=n || fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){close(fd);return -1;}
    return fd;
}
static int test_mode(const char *mode,unsigned expected) {
    int fd=fixture(mode);CHECK(fd>=0);
    struct core c={0}; dc_config cfg; uint64_t epoch=123456;
    CHECK(setenv("MODEL_BARGE_ANY",expected?"0":"1",1)==0);
    CHECK(configure_account(&c,&cfg,epoch,fd)==0);
    CHECK(cfg.barge_any==expected);
    dc_session_ops ops={NULL,noop_tag,noop_tag,noop_question,noop_close,noop_light};
    CHECK(dc_session_open(&c.session,epoch,&cfg,&ops,"",1)==0);
    CHECK(c.session.control.config.barge_any==expected);
    dc_tag right={c.session.control.tag.session_epoch,1,1}, wrong={epoch+1,1,1};
    CHECK(dc_account_start(c.account,wrong,"question","/nonexistent-synthetic","model",now_ms())<0);
    CHECK(dc_account_start(c.account,right,"question","/nonexistent-synthetic","model",now_ms())==0);
    int got=0,done=0;uint64_t until=now_ms()+3000;
    while(now_ms()<until && !done) {
        dc_account_event event;int r=dc_account_poll(c.account,now_ms(),&event);CHECK(r>=0);
        if(r && event.type==DC_ACCOUNT_DELTA && !strcmp(event.text,"bound"))got=1;
        if(r && event.type==DC_ACCOUNT_DONE)done=1;
        if(r && event.type==DC_ACCOUNT_ERROR)break;
        usleep(1000);
    }
    CHECK(got && done);CHECK(dc_account_destroy(c.account)==0);close(fd);return 0;
}
int main(void) {
    CHECK(test_mode("keyword",0)==0);CHECK(test_mode("voice",1)==0);
    struct core c={0};dc_config cfg;
    CHECK(setenv("MODEL_BARGE_ANY","1",1)==0);
    CHECK(configure_account(&c,&cfg,77,-1)==0);CHECK(cfg.barge_any==1);
    CHECK(dc_account_destroy(c.account)==0);
    memset(&c,0,sizeof c);CHECK(setenv("MODEL_BARGE_ANY","0",1)==0);
    CHECK(configure_account(&c,&cfg,77,-1)==0);CHECK(cfg.barge_any==0);
    CHECK(dc_account_destroy(c.account)==0);
    int fd=fixture("unknown");CHECK(fd>=0);memset(&c,0,sizeof c);
    CHECK(configure_account(&c,&cfg,77,fd)<0);CHECK(c.account==NULL);close(fd);
    fd=memfd_create("unsealed",MFD_ALLOW_SEALING);CHECK(fd>=0);memset(&c,0,sizeof c);
    CHECK(configure_account(&c,&cfg,77,fd)<0);CHECK(c.account==NULL);close(fd);
    puts("account mode binding: PASS");return 0;
}
