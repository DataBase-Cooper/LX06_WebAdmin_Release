#include "greeting_identity.h"
#include <string.h>

static int known(const uint8_t id[DC_GREETING_ID_BYTES]) {
    unsigned value = 0;
    for (size_t i = 0; i < DC_GREETING_ID_BYTES; ++i) value |= id[i];
    return value != 0;
}

static int valid(const dc_greeting_identity *v) {
    return v && v->version == 1 && known(v->provider) && known(v->account) &&
        known(v->voice) && v->epoch && v->voice_revision && v->profile_revision &&
        v->rate == 16000 && v->channels == 1 && v->sample_format == DC_GREETING_S16LE &&
        v->text && v->text_len && v->text_len <= 240 && !memchr(v->text, 0, v->text_len);
}

int dc_greeting_identity_match(const dc_greeting_identity *a, const dc_greeting_identity *b) {
    if (!valid(a) || !valid(b)) return 0;
    return a->epoch == b->epoch && a->voice_revision == b->voice_revision &&
        a->profile_revision == b->profile_revision &&
        !memcmp(a->provider,b->provider,DC_GREETING_ID_BYTES) &&
        !memcmp(a->account,b->account,DC_GREETING_ID_BYTES) &&
        !memcmp(a->voice,b->voice,DC_GREETING_ID_BYTES) &&
        a->text_len == b->text_len && !memcmp(a->text,b->text,a->text_len);
}
