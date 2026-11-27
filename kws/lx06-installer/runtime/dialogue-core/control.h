/* One provider-independent control state machine; never performs I/O. */
#ifndef LX06_DC_CONTROL_H
#define LX06_DC_CONTROL_H
#include "types.h"
#include <stddef.h>
#define DC_WORDS_CAP 512
#define DC_QUESTION_CAP 2048
enum dc_action {
    DC_ACT_NONE=0, DC_ACT_STOP=1, DC_ACT_CANCEL=2, DC_ACT_END=4,
    DC_ACT_QUESTION=8, DC_ACT_STATE=16
};
typedef struct dc_config {
    char interrupt[DC_WORDS_CAP], goodbye[DC_WORDS_CAP];
    unsigned barge_any, idle_ms, tail_ms;
    double gain_db;
} dc_config;
typedef struct dc_control {
    dc_tag tag;
    enum dc_state state;
    dc_config config;
    uint64_t next_generation, last_cancelled, interrupted_utterance;
    uint64_t last_final_utterance, last_activity_ms, tail_until_ms;
    unsigned reply_live, closed;
} dc_control;
typedef struct dc_decision {
    unsigned actions;
    unsigned interrupt_matched; /* validated, admitted non-goodbye text */
    dc_tag cancelled, current;
    enum dc_state state;
    char question[DC_QUESTION_CAP];
} dc_decision;
/* Validate without modifying configuration or silently truncating tokens. */
int dc_config_valid(const dc_config *);
int dc_control_open(dc_control *, uint64_t epoch, const dc_config *, uint64_t now);
dc_tag dc_control_reply(dc_control *, uint64_t utterance, uint64_t now);
int dc_control_state(dc_control *, dc_tag, enum dc_state, uint64_t now);
/* source identifies the original recognizer utterance, not the most recently
 * started response. A partial is never submitted as a model question. */
int dc_control_text(dc_control *, dc_tag source, const char *text, int final,
                    int echo, uint64_t now, dc_decision *);
int dc_control_voice(dc_control *, dc_tag source, int near_end, int echo,
                     uint64_t now, dc_decision *);
int dc_control_finish_reply(dc_control *, dc_tag, uint64_t now);
int dc_control_close(dc_control *, uint64_t now, dc_decision *);
/* close() first enters CLOSING: the session is no longer callable, but its
 * light remains active. Only after all owned I/O is reclaimed may the core
 * commit CLOSED and emit the ending effect. */
int dc_control_finish_close(dc_control *, uint64_t now, dc_decision *);
int dc_control_tick(dc_control *, uint64_t now, dc_decision *);
/* Delete only configured control spans; retain the entire remaining utterance.
 * UTF-8 punctuation/space may separate characters in a configured phrase. */
int dc_control_strip(char *out, size_t cap, const char *text, const char *words);
#endif
