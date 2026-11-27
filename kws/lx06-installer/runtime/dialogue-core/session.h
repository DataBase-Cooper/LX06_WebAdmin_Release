/* The sole policy/side-effect coordinator. Providers never own these rules. */
#ifndef LX06_DC_SESSION_H
#define LX06_DC_SESSION_H
#include "control.h"
typedef struct dc_session_ops {
    void *context;
    void (*stop_local)(void *,dc_tag);
    void (*cancel_remote)(void *,dc_tag);
    int (*question)(void *,dc_tag,const char *complete_question);
    void (*close_remote)(void *);
    void (*light)(void *,dc_tag,uint64_t sequence,enum dc_state);
} dc_session_ops;
typedef struct dc_session {
    dc_control control;
    dc_session_ops ops;
    uint64_t light_sequence,echo_until_ms;
    enum dc_state emitted_state;
    char recent[1441],greeting[241],wake[97];
    dc_tag greeting_tag;
} dc_session;
int dc_session_open(dc_session *,uint64_t,const dc_config *,const dc_session_ops *,const char *wake,uint64_t);
int dc_session_ready(dc_session *,uint64_t);
dc_tag dc_session_reply(dc_session *,uint64_t utterance,uint64_t);
dc_tag dc_session_greeting(dc_session *,const char *,uint64_t);
/* Returns <0 on invalid input or a failed question callback (which closes the
 * session), 0 if ignored, and >0 if a decision was applied. */
int dc_session_text(dc_session *,dc_tag,const char *,int final,uint64_t);
/* Reports an admitted, validated interruption even when no reply is live. */
int dc_session_text_interrupt(dc_session *,dc_tag,const char *,int final,uint64_t,int *matched);
int dc_session_voice(dc_session *,dc_tag,int near_end,int echo,uint64_t);
int dc_session_speaking(dc_session *,dc_tag,uint64_t);
int dc_session_waiting(dc_session *,dc_tag,uint64_t);
int dc_session_reply_done(dc_session *,dc_tag,uint64_t);
void dc_session_spoken(dc_session *,dc_tag,const char *,uint64_t);
int dc_session_close(dc_session *,uint64_t);
int dc_session_finish_close(dc_session *,uint64_t);
int dc_session_tick(dc_session *,uint64_t);
#endif
