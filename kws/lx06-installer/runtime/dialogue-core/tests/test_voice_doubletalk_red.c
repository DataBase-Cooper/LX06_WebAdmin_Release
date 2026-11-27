/* Offline synthetic counterexample using the real guard and bundled libfvad.
 * This is an intentionally RED acceptance test, not proof of acoustic AEC.
 * Run on Linux: gcc -std=c11 -O2 -Wall -Wextra -Werror -Ivendor/libfvad/include
 * -Ivendor/libfvad/src tests/test_voice_doubletalk_red.c voice_guard.c
 * control.c ../doubao/src/text_match.c $(find vendor/libfvad/src -name '*.c')
 * -lm -pthread -o "$TMPDIR/dc-doubletalk"
 * && "$TMPDIR/dc-doubletalk"  (from runtime/dialogue-core).
 */
#include "../voice_guard.h"
#include "../control.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error This fixture requires checks enabled
#endif
static unsigned failures;
#define CHECK(c, name) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n", name); failures++; } else printf("PASS: %s\n", name); } while (0)

static dc_voice_result run(int kind) {
    dc_voice_guard *g=dc_voice_guard_create();
    dc_voice_result r={0}; int16_t ref[480],in[480],out[480];
    if (!g) {fprintf(stderr,"guard allocation failed\n");exit(2);}
    for (unsigned i=0;i<480;i++) {
        double t=(double)i/16000.0;
        ref[i]=(int16_t)(3500*sin(2*3.141592653589793*220*t)+1500*sin(2*3.141592653589793*330*t));
    }
    for (unsigned k=0;k<6;k++) {
        for (unsigned i=0;i<480;i++) {
            double t=(double)i/16000.0;
            int near=(int)(1300*sin(2*3.141592653589793*510*t)+700*sin(2*3.141592653589793*690*t));
            in[i]=(int16_t)(kind==0?0:kind==1?ref[i]:kind==2?near:kind==3?ref[i]+near:ref[i]);
        }
        uint64_t now=1000+30*k;
        if (kind==1||kind==3) dc_voice_guard_reference(g,ref,480,16000,now);
        if (kind==4&&k==0) dc_voice_guard_reference(g,ref,480,16000,100);
        if (kind==5) dc_voice_guard_reference(g,ref,480,16000,now+1000);
        if (dc_voice_guard_process(g,in,480,0,out,now,&r)!=0) {fprintf(stderr,"guard process failed\n");exit(2);}
    }
    dc_voice_guard_destroy(g);return r;
}
static unsigned stop_actions(dc_voice_result r) {
    dc_control c; dc_config conf={.barge_any=1}; dc_decision d={0};
    if (dc_control_open(&c,42,&conf,0)) exit(2);
    dc_tag reply=dc_control_reply(&c,1,10);
    if (!reply.reply_generation) exit(2);
    /* Match the production capture branch; recognizer identity is supplied. */
    if (r.near_end && dc_control_voice(&c,(dc_tag){42,reply.reply_generation,2},1,r.echo,1000,&d)<0) exit(2);
    return d.actions;
}
int main(void) {
    dc_voice_result silent=run(0),playback=run(1),near=run(2),mixed=run(3),late=run(4),future=run(5);
    printf("silence voiced=%u near=%u echo=%u\n",silent.voiced,silent.near_end,silent.echo);
    printf("playback voiced=%u near=%u echo=%u corr=%.3f\n",playback.voiced,playback.near_end,playback.echo,playback.correlation);
    printf("near voiced=%u near=%u echo=%u\n",near.voiced,near.near_end,near.echo);
    printf("mixed voiced=%u near=%u echo=%u corr=%.3f\n",mixed.voiced,mixed.near_end,mixed.echo,mixed.correlation);
    printf("late voiced=%u near=%u echo=%u corr=%.3f\n",late.voiced,late.near_end,late.echo,late.correlation);
    CHECK(!silent.near_end&&!silent.echo,"silence does not interrupt");
    CHECK(playback.echo&&!playback.near_end,"matched playback does not interrupt");
    CHECK(near.near_end&&!near.echo,"independent near-end signal qualifies alone");
    CHECK(mixed.near_end&&!mixed.echo,"same near-end signal must interrupt during matching playback");
    CHECK(!late.echo&&late.correlation==0,"expired reference is not considered current");
    CHECK(!future.echo&&future.correlation==0,"future reference is not considered current");
    CHECK(!(stop_actions(playback)&DC_ACT_STOP),"playback alone produces no control stop");
    CHECK((stop_actions(near)&(DC_ACT_STOP|DC_ACT_CANCEL))==(DC_ACT_STOP|DC_ACT_CANCEL),"near-end alone reaches stop and cancel");
    CHECK((stop_actions(mixed)&(DC_ACT_STOP|DC_ACT_CANCEL))==(DC_ACT_STOP|DC_ACT_CANCEL),"double-talk reaches stop and cancel");
    return failures?1:0;
}
