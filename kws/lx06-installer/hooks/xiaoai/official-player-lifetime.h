#ifndef LX06_OFFICIAL_PLAYER_LIFETIME_H
#define LX06_OFFICIAL_PLAYER_LIFETIME_H
#include <stdint.h>

/* This process-local source covers observed libvlc player object lifetimes.
 * It does not cover SDK pending work, official sessions, or global admission.
 * An unknown library is forwarded unchanged but yields UNKNOWN and no token.
 * Supported identity means the exact installed OEM image and matching loaded
 * read-only code, pinned for this process; self-modifying code is unsupported.
 * A token belongs to this process; never serialize it as a global owner ID. */
enum lx06_player_lifetime_state {
    LX06_PLAYER_LIVE = 1,
    LX06_PLAYER_STOP_RETURNED,
    LX06_PLAYER_RELEASING,
    LX06_PLAYER_RETIRED,
    LX06_PLAYER_UNKNOWN
};
struct lx06_player_lifetime {
    uint64_t generation;
    uint64_t media_generation;
    unsigned references;
    unsigned in_flight;
    enum lx06_player_lifetime_state state;
};
struct lx06_player_lifetime_totals {
    unsigned occupied;
    unsigned in_flight;
    unsigned unknown;
};
/* Obtain a token while the caller owns this opaque player handle. */
int lx06_official_player_token(const void *handle, uint64_t *generation);
/* A missing/stale token returns 0, never an assertion that anything is idle. */
int lx06_official_player_read(uint64_t generation, struct lx06_player_lifetime *out);
void lx06_official_player_totals(struct lx06_player_lifetime_totals *out);
#endif
