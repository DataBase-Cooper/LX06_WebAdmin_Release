/* Offline counterexample, not a production implementation or ALSA emulator.
 * The mock permits a hidden XRUN followed by recovery, without asserting that
 * the target ALSA stack actually does so. Compile with a C99 compiler. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct _snd_pcm { int state, hidden, inject, calls; long result; } snd_pcm_t;
enum { PREPARED=2, RUNNING=3, XRUN=4 };
enum { NONE, INSIDE_WRITE, BETWEEN_WRITES, VISIBLE_IN_WRITE };
static int snd_pcm_state(snd_pcm_t *p) { return p->state; }
static long snd_pcm_writei(snd_pcm_t *p, const void *data, unsigned long frames) {
    (void)data; (void)frames; p->calls++;
    if (p->inject==INSIDE_WRITE) { p->hidden++; p->state=XRUN; p->state=RUNNING; p->inject=NONE; }
    if (p->inject==VISIBLE_IN_WRITE) { p->state=XRUN; p->inject=NONE; }
    return p->result;
}
typedef struct { uint64_t next; int active; } seam;
typedef struct { int before, after; long written; uint64_t id; int joined; } event;
/* Proposal under test: sample before/after and label successful writes.  A
 * bounded ID never wraps; a zero ID explicitly means no continuity evidence. */
static event checked_write(seam *s, snd_pcm_t *p) {
    int16_t data[160]={0}; event e; memset(&e,0,sizeof e);
    e.before=snd_pcm_state(p);
    e.written=snd_pcm_writei(p,data,160);
    e.after=snd_pcm_state(p);
    if (e.written>0 && e.written<=160 && s->next) {
        e.id=s->next;
        s->next=(s->next==UINT64_MAX)?0:s->next+1;
        e.joined=s->active && e.before==RUNNING && e.after==RUNNING;
        s->active=(e.after==RUNNING && s->next!=0);
    } else s->active=0;
    return e;
}
static void hidden_between(snd_pcm_t *p) {
    if(p->inject==BETWEEN_WRITES) { p->hidden++; p->state=XRUN; p->state=RUNNING; p->inject=NONE; }
}
static void run_pair(int inject, event out[2], int *hidden) {
    snd_pcm_t p={RUNNING,0,NONE,0,160}; seam s={41,1};
    out[0]=checked_write(&s,&p); p.inject=inject; hidden_between(&p); out[1]=checked_write(&s,&p);
    CHECK(p.calls==2); *hidden=p.hidden;
}
static int same_event(event a,event b) {
    return a.before==b.before && a.after==b.after && a.written==b.written && a.id==b.id && a.joined==b.joined;
}
int main(int argc,char **argv) {
    event normal[2], inside[2], between[2]; int n,i,b;
    run_pair(NONE,normal,&n); run_pair(INSIDE_WRITE,inside,&i); run_pair(BETWEEN_WRITES,between,&b);
    CHECK(n==0 && i==1 && b==1);
    for(int j=0;j<2;j++) { CHECK(same_event(normal[j],inside[j])); CHECK(same_event(normal[j],between[j])); }
    CHECK(normal[1].joined && inside[1].joined && between[1].joined);
    puts("PASS indistinguishability: clean, hidden-in-write, hidden-between-writes have identical observations and IDs");
    snd_pcm_t p={XRUN,0,NONE,0,160}; seam s={41,1}; event e=checked_write(&s,&p);
    CHECK(!e.joined && !s.active && e.id==41);
    p.state=RUNNING; p.inject=VISIBLE_IN_WRITE; s.active=1; e=checked_write(&s,&p);
    CHECK(e.before==RUNNING && e.after==XRUN && !e.joined && !s.active);
    p.state=RUNNING; p.result=-EPIPE; s.active=1; e=checked_write(&s,&p);
    CHECK(!e.joined && !s.active && e.id==0);
    p.result=-ESTRPIPE; s.active=1; e=checked_write(&s,&p); CHECK(!e.joined && !s.active);
    p.result=-EINTR; s.active=1; e=checked_write(&s,&p); CHECK(!e.joined && !s.active);
    p.result=-EAGAIN; s.active=1; e=checked_write(&s,&p); CHECK(!e.joined && !s.active);
    p.result=0; s.active=1; e=checked_write(&s,&p); CHECK(!e.joined && !s.active);
    p.result=161; s.active=1; e=checked_write(&s,&p); CHECK(!e.joined && !s.active);
    p.result=160; p.state=RUNNING; s.next=UINT64_MAX; s.active=1; e=checked_write(&s,&p);
    CHECK(e.id==UINT64_MAX && !s.active && s.next==0);
    e=checked_write(&s,&p); CHECK(!e.joined && !e.id && !s.active);
    puts("PASS visible XRUN, EPIPE, EAGAIN, zero, invalid length and bounded-ID exhaustion fence");
    if(argc==2 && !strcmp(argv[1],"--require-no-hidden-join")) {
        CHECK(!inside[1].joined && !between[1].joined);
    }
    return 0;
}
