#ifndef DC_GREETING_LOAD_H
#define DC_GREETING_LOAD_H
#include "greeting_identity.h"
#define DC_GREETING_WAV_MAX (96u * 1024u)
/* Offline, in-memory validation seam only; no file/network/locking/allocations.
 * The caller must authenticate BOTH identities and bind cached metadata and WAV
 * to one immutable trusted snapshot before this call. Matching is not proof of
 * authenticity or freshness. Input, output and out_len must not overlap and
 * remain stable for the call. Never pass an untrusted pathname or descriptor.
 * Copies only canonical 16 kHz mono s16le PCM on exact identity match.
 * Returns 1 on hit; on miss returns 0 and sets *out_len=0 if non-NULL, without
 * touching out. No playback, fallback, or session integration is performed. */
int dc_greeting_load(const dc_greeting_identity *requested,
                     const dc_greeting_identity *cached,
                     const unsigned char *wav, size_t wav_len,
                     unsigned char *out, size_t out_cap, size_t *out_len);
#endif
