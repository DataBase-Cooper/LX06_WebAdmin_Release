/* Offline-only candidate contract; no production callers. */
#include "residual_candidate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    residual_candidate *c=residual_candidate_new();
    CHECK(c);
    int16_t mic[480], residual[480];
    memset(mic,0,sizeof mic);
    for(int i=0;i<480;i++) residual[i]=30000;
    int trigger=-1;
    CHECK(residual_candidate_process(c,mic,residual,480,1000,&trigger)==0);
    CHECK(!trigger);
    CHECK(residual_candidate_process(c,mic,residual,480,1000,&trigger)<0);
    CHECK(residual_candidate_process(c,mic,residual,480,999,&trigger)<0);
    CHECK(residual_candidate_process(c,mic,residual,480,1030,&trigger)==0);
    CHECK(!trigger);
    for(int i=0;i<480;i++) mic[i]=residual[i]=1000;
    CHECK(residual_candidate_process(c,mic,residual,480,1730,&trigger)==0);
    residual_candidate_free(c);
    puts("candidate contract PASS");
    return 0;
}
