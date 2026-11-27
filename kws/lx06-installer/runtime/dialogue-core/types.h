/* Shared identities and states. Only the dialogue core allocates identities. */
#ifndef LX06_DC_TYPES_H
#define LX06_DC_TYPES_H
#include <stdint.h>
typedef struct dc_tag {
    uint64_t session_epoch;
    uint64_t reply_generation;
    uint64_t utterance_id;
} dc_tag;
static inline int dc_tag_equal(dc_tag a, dc_tag b) {
    return a.session_epoch == b.session_epoch &&
           a.reply_generation == b.reply_generation &&
           a.utterance_id == b.utterance_id;
}
enum dc_state {
    DC_CLOSED = 0, DC_CONNECTING, DC_LISTENING, DC_THINKING,
    DC_SPEAKING, DC_INTERRUPTED, DC_CLOSING
};
#endif
