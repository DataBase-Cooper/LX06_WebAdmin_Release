#ifndef DC_GREETING_IDENTITY_H
#define DC_GREETING_IDENTITY_H
#include <stddef.h>
#include <stdint.h>

#define DC_GREETING_ID_BYTES 32
#define DC_GREETING_S16LE 1

/* Value comparison only: callers MUST obtain both tuples through a separately
 * authenticated, authoritative source. Tokens are opaque nonsecret identifiers,
 * never credentials, account settings or digests of credentials. Zero is unknown.
 * The caller retains text storage for the duration of the call. */
typedef struct {
    uint32_t version;
    uint8_t provider[DC_GREETING_ID_BYTES];
    uint8_t account[DC_GREETING_ID_BYTES];
    uint8_t voice[DC_GREETING_ID_BYTES];
    uint64_t epoch, voice_revision, profile_revision;
    uint32_t rate, channels, sample_format;
    const unsigned char *text;
    size_t text_len;
} dc_greeting_identity;

/* Returns 1 only for complete, supported and byte-exact tuples, otherwise 0.
 * This does not authenticate, persist, load or authorize playback of anything. */
int dc_greeting_identity_match(const dc_greeting_identity *a, const dc_greeting_identity *b);
#endif
