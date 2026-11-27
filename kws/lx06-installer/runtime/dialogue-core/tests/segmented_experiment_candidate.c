/* Isolated offline-only proposal. Single-threaded harness; NOT a player fence. */
#include "../voice_guard.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct segmented {dc_voice_guard *guard;uint64_t epoch,gen;} segmented;
segmented *seg_create(void){segmented *c=calloc(1,sizeof *c);if(c){c->guard=dc_voice_guard_create();if(!c->guard){free(c);return NULL;}}return c;}
void seg_free(segmented *c){if(c){dc_voice_guard_destroy(c->guard);free(c);}}
int seg_begin(segmented *c,uint64_t epoch,uint64_t gen){
 if(!c||!epoch||!gen||(epoch<c->epoch)||(epoch==c->epoch&&gen<=c->gen))return -1;
 dc_voice_guard *fresh=dc_voice_guard_create();if(!fresh)return -1;
 dc_voice_guard_destroy(c->guard);c->guard=fresh;c->epoch=epoch;c->gen=gen;return 0;
}
int seg_reference(segmented *c,const int16_t *p,size_t n,uint64_t now,uint64_t epoch,uint64_t gen){
 if(!c||!p||!c->epoch||epoch!=c->epoch||gen!=c->gen||!n||n>160)return -1;
 dc_voice_guard_reference(c->guard,p,n,16000,now);return 0;
}
int seg_process_fenced(segmented *c,const int16_t *p,uint64_t now,uint64_t epoch,uint64_t gen,dc_voice_result *out){
 if(!c||!c->epoch||epoch!=c->epoch||gen!=c->gen||!p||!out)return -1;
 int16_t audio[160];dc_voice_result r;unsigned all=1,near=0;
 for(int i=0;i<3;i++){
  if(dc_voice_guard_process(c->guard,p+160*i,160,0,audio,now,&r))return -1;
  all &= r.echo;near |= r.near_end;
 }
 *out=r;out->echo=all;out->near_end=near;return 0;
}
