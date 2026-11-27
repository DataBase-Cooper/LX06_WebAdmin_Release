/* Offline integration of the actual reactor stop callback and player worker. */
#define main dc_unused_program_main
#include "../main.c"
#undef main
#include <stdatomic.h>
#ifdef NDEBUG
#error This fixture requires assertions
#endif
#include <assert.h>

struct sink { atomic_int blocked, hold_drop, drop_entered, fail_drop; atomic_uint frames, drops, attempts; };
static int open_sink(void *p,unsigned r){(void)p;(void)r;return 0;}
static int poll_sink(void *p,struct pollfd *f,unsigned n){(void)p;(void)f;(void)n;return 0;}
static ssize_t write_sink(void *p,const int16_t *s,size_t n){struct sink *x=p;(void)s;atomic_fetch_add(&x->attempts,1);if(atomic_load(&x->blocked))return -EAGAIN;atomic_fetch_add(&x->frames,(unsigned)n);return (ssize_t)n;}
static int delay_sink(void *p,long *d){(void)p;*d=0;return 0;}
static int prepare_sink(void *p){(void)p;return 0;}
static int drop_sink(void *p){struct sink *x=p;atomic_fetch_add(&x->drops,1);atomic_store(&x->drop_entered,1);while(atomic_load(&x->hold_drop))usleep(1000);return atomic_load(&x->fail_drop)?-EIO:0;}
static void close_sink(void *p){(void)p;}
static void await_event(dc_io_player *p,enum dc_io_event_type type,dc_tag tag){
    uint64_t end=now_ms()+3000;dc_io_event e;
    while(now_ms()<end){while(dc_io_player_next_event(p,&e)>0){assert(e.type!=DC_IO_ERROR);if(e.type==type){assert(dc_tag_equal(e.tag,tag));return;}}usleep(1000);}assert(!"event timeout");
}
struct scenario {struct core *core; unsigned cancels, questions; char question[DC_QUESTION_CAP]; dc_tag cancelled;};
static void scenario_stop(void *v,dc_tag t){struct scenario *s=v;stop_local(s->core,t);}
static void scenario_cancel(void *v,dc_tag t){struct scenario *s=v;assert(s->core->stopping_player);s->cancelled=t;s->cancels++;}
static int scenario_question(void *v,dc_tag t,const char *q){struct scenario *s=v;(void)t;s->questions++;strcpy(s->question,q);return 0;}
static void scenario_close(void *v){(void)v;}
static void scenario_light(void *v,dc_tag t,uint64_t seq,enum dc_state state){(void)v;(void)t;(void)seq;(void)state;}
static void integration(struct core *c,struct sink *sink){
    struct scenario s={.core=c};dc_config config={0};
    strcpy(config.interrupt,"等一下,等等,你说得不对");strcpy(config.goodbye,"结束对话,拜拜了哦");
    dc_session_ops ops={&s,scenario_stop,scenario_cancel,scenario_question,scenario_close,scenario_light};
    assert(dc_session_open(&c->session,11,&config,&ops,"",100)==0);
    assert(dc_session_ready(&c->session,101)==0);
    dc_tag old=dc_session_reply(&c->session,1,102);int16_t pcm[320]={0};
    atomic_store(&sink->blocked,1);atomic_store(&sink->hold_drop,1);atomic_store(&sink->drop_entered,0);
    atomic_store(&sink->attempts,0);
    assert(begin_player(c,old,16000)==0);assert(dc_io_player_write(c->player,old,pcm,sizeof pcm)==0);
    uint64_t end=now_ms()+3000;while(!atomic_load(&sink->attempts)&&now_ms()<end)usleep(1000);assert(atomic_load(&sink->attempts));
    dc_tag source={11,old.reply_generation,2};
    assert(dc_session_text(&c->session,source,"等一下",0,103)==1);
    assert(s.cancels==1&&dc_tag_equal(s.cancelled,old)&&c->stopping_player&&!c->player_live);
    assert(dc_session_text(&c->session,source,"等等，你说得不对。完整问题",1,104)==1);
    assert(s.cancels==1&&s.questions==1&&!strcmp(s.question,"完整问题"));
    dc_tag next=c->session.control.tag;
    assert(begin_player(c,next,16000)==-EAGAIN);
    end=now_ms()+3000;while(!atomic_load(&sink->drop_entered)&&now_ms()<end)usleep(1000);
    assert(atomic_load(&sink->drop_entered));player_events(c,now_ms());assert(c->stopping_player);
    assert(dc_session_text(&c->session,source,"结束对话",1,105)==0);
    assert(dc_session_text(&c->session,(dc_tag){11,next.reply_generation,2},"结束对话",0,105)==0);
    assert(!c->session.control.closed&&dc_tag_equal(c->session.control.tag,next));
    atomic_store(&sink->hold_drop,0);
    end=now_ms()+3000;while(c->stopping_player&&now_ms()<end){player_events(c,now_ms());usleep(1000);}assert(!c->stopping_player);
    unsigned before=atomic_load(&sink->frames);atomic_store(&sink->blocked,0);
    assert(begin_player(c,next,16000)==0);assert(dc_io_player_write(c->player,next,pcm,sizeof pcm)==0);
    assert(dc_io_player_finish(c->player,next)==0);await_event(c->player,DC_IO_DRAINED,next);
    assert(atomic_load(&sink->frames)==before+320);c->player_live=0;
}
int main(void){
    struct sink sink={0};dc_io_playback_backend backend={&sink,open_sink,poll_sink,write_sink,delay_sink,prepare_sink,drop_sink,close_sink};
    struct core c={0};c.player=dc_io_player_create(&backend);assert(c.player);
    dc_tag old={1,1,1},newer={2,1,1};int16_t pcm[320]={0};
    atomic_store(&sink.blocked,1);
    assert(begin_player(&c,old,16000)==0);assert(dc_io_player_write(c.player,old,pcm,sizeof pcm)==0);
    c.player_sealed=1;c.have_audio=1;c.pending_audio.header.epoch=old.session_epoch;c.pending_audio.header.generation=old.reply_generation;
    stop_local(&c,old);assert(!c.player_live&&!c.player_sealed&&!c.have_audio&&c.stopping_player);
    c.pending_audio.header.epoch=newer.session_epoch;c.pending_audio.header.generation=newer.reply_generation;c.have_audio=1;
    stop_local(&c,old);assert(c.stopping_player && c.have_audio);
    {uint64_t end=now_ms()+3000;while(c.stopping_player&&now_ms()<end){player_events(&c,now_ms());usleep(1000);}assert(!c.stopping_player);}
    stop_local(&c,old);assert(!c.stopping_player && c.have_audio);
    assert(begin_player(&c,newer,16000)==0);assert(dc_io_player_write(c.player,newer,pcm,sizeof pcm)==0);
    c.player_sealed=1;c.have_audio=1;
    stop_local(&c,old);
    assert(c.player_live && c.player_sealed && c.have_audio && !c.stopping_player);
    c.player_sealed=0;atomic_store(&sink.blocked,0);
    assert(dc_io_player_finish(c.player,newer)==0);await_event(c.player,DC_IO_DRAINED,newer);
    assert(atomic_load(&sink.frames)==320);
    /* DRAINED was consumed directly above; mirror its reactor bookkeeping. */
    c.player_live=0;
    assert(dc_media_cue_begin(&c.cue,newer.session_epoch,NULL)==0);
    assert(dc_io_player_begin(c.player,c.cue.tag,DC_MEDIA_RATE)==0);c.cue_player=1;
    stop_local(&c,old);assert(c.cue_player && dc_media_cue_active(&c.cue));
    stop_local(&c,newer);assert(!c.cue_player && !dc_media_cue_active(&c.cue) && c.stopping_player);
    {uint64_t end=now_ms()+3000;while(c.stopping_player&&now_ms()<end){player_events(&c,now_ms());usleep(1000);}assert(!c.stopping_player);}
    integration(&c,&sink);
    /* A stop error is tagged with the interrupted owner, not the new session tag. */
    dc_tag interrupted={19,1,1};c.session.control.tag=(dc_tag){19,2,2};
    atomic_store(&sink.attempts,0);atomic_store(&sink.fail_drop,1);atomic_store(&sink.blocked,1);
    assert(!begin_player(&c,interrupted,16000));assert(!dc_io_player_write(c.player,interrupted,pcm,sizeof pcm));
    {uint64_t end=now_ms()+3000;while(!atomic_load(&sink.attempts)&&now_ms()<end)usleep(1000);assert(atomic_load(&sink.attempts));}
    stop_local(&c,interrupted);
    {uint64_t end=now_ms()+3000;while(!c.failed&&now_ms()<end){player_events(&c,now_ms());usleep(1000);}assert(c.failed);}
    assert(c.stopping_player);assert(dc_io_player_begin(c.player,(dc_tag){20,1,1},16000)==-EIO);
    dc_io_player_destroy(c.player);puts("reactor player stale-stop: PASS");return 0;
}
