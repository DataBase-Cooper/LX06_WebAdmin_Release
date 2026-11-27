#include "greeting_load.h"
#include <string.h>

static unsigned read_le(const unsigned char *p, unsigned n) {
    unsigned v=0;
    for (unsigned i=0;i<n;i++) v |= (unsigned)p[i] << (8*i);
    return v;
}

int dc_greeting_load(const dc_greeting_identity *requested,
                     const dc_greeting_identity *cached,
                     const unsigned char *wav, size_t wav_len,
                     unsigned char *out, size_t out_cap, size_t *out_len) {
    if (!out_len) return 0;
    *out_len=0;
    if (!out || !wav || wav_len<46 || wav_len>DC_GREETING_WAV_MAX ||
        !dc_greeting_identity_match(requested,cached)) return 0;
    size_t pcm_len=wav_len-44;
    if (!pcm_len || (pcm_len & 1) || pcm_len>out_cap ||
        memcmp(wav,"RIFF",4) || read_le(wav+4,4)!=wav_len-8 ||
        memcmp(wav+8,"WAVEfmt ",8) || read_le(wav+16,4)!=16 ||
        read_le(wav+20,2)!=1 || read_le(wav+22,2)!=1 ||
        read_le(wav+24,4)!=16000 || read_le(wav+28,4)!=32000 ||
        read_le(wav+32,2)!=2 || read_le(wav+34,2)!=16 ||
        memcmp(wav+36,"data",4) || read_le(wav+40,4)!=pcm_len) return 0;
    memcpy(out,wav+44,pcm_len);
    *out_len=pcm_len;
    return 1;
}
