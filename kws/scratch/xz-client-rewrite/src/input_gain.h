#ifndef XZ_INPUT_GAIN_H
#define XZ_INPUT_GAIN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

/* Uplink-only presets. Never alter capture/AEC, control-word recognition, or
 * playback. Chinese values match the script's existing select-field protocol;
 * numeric/ASCII aliases are convenient for direct CLI use. Missing/invalid is
 * an exact bypass, preserving previously deployed scripts. */
typedef struct { unsigned preset; float current,target,db; } xz_input_gain;

static inline unsigned xz_input_gain_preset(const char *value) {
    if (!value) return 0;
    if (!strcmp(value,"低增益") || !strcmp(value,"low") || !strcmp(value,"1")) return 1;
    if (!strcmp(value,"中增益") || !strcmp(value,"medium") || !strcmp(value,"2")) return 2;
    if (!strcmp(value,"高增益") || !strcmp(value,"high") || !strcmp(value,"3")) return 3;
    return 0;
}

static inline void xz_input_gain_init(xz_input_gain *state, const char *value) {
    state->preset=xz_input_gain_preset(value); state->current=1.0f;
    static const float levels[]={1.0f,1.5f,2.0f,3.0f};
    state->target=levels[state->preset];state->db=20.0f*log10f(state->target);
}

/* Signed dB is attenuation/amplification, never a negative PCM multiplier.
 * Explicit decimal setting wins; missing retains old deployed preset values.
 * Do not alter AEC/control-word/barge-in decisions or playback sensitivity. */
static inline void xz_input_gain_init_db(xz_input_gain *state,const char *value,const char *legacy) {
    xz_input_gain_init(state,legacy);
    if(!value||!*value)return;
    xz_input_gain_init(state,NULL);
    const char *p=value;int digits=0,decimals=0,dot=0;
    if(*p=='+'||*p=='-')p++;
    for(;*p;p++) {
        if(*p=='.'&&!dot){dot=1;continue;}
        if(*p<'0'||*p>'9'||(dot&&++decimals>2))return;
        digits++;
    }
    if(!digits)return;
    char *end;errno=0;double db=strtod(value,&end);
    if(errno||*end||!isfinite(db)||db<-12||db>12)return;
    state->db=(float)db;state->target=powf(10.0f,state->db/20.0f);
    state->current=state->target<1.0f?state->target:1.0f;
}

static inline void xz_input_gain_apply(xz_input_gain *state, int16_t *pcm,
                                     size_t count, int listening) {
    if (state->target==1.0f || !listening) { state->current=1.0f; return; }
    int peak=0;
    for(size_t i=0;i<count;i++) {
        int value=pcm[i]; if(value<0)value=-value;
        if(value>peak)peak=value;
    }
    float target=state->target;
    /* One gain for the whole frame preserves its waveform. Peak headroom
     * prevents overflow/flat-top clipping, with immediate attenuation and
     * slower recovery (~several 60 ms frames) to avoid gain pumping. */
    if(peak && target*peak>30000.0f)target=30000.0f/peak;
    state->current=target<state->current ? target :
                   state->current+0.2f*(target-state->current);
    for(size_t i=0;i<count;i++)pcm[i]=(int16_t)((float)pcm[i]*state->current);
}
#endif
