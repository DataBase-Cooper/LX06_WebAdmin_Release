#ifndef LX06_OFFICIAL_PLAYER_IDENTITY_H
#define LX06_OFFICIAL_PLAYER_IDENTITY_H
#include <stddef.h>
/* Verify the image containing every named API pointer against a caller-owned
 * fixed SHA256 and size, plus its loaded read-only PT_LOAD bytes. The returned
 * dlopen reference pins that same loaded image; caller owns dlclose. No player
 * object layout, request identity, callback or retirement is inferred here. */
int lx06_player_elf_exact(const void *const *, size_t, const unsigned char[32],
                          size_t, void **);
#endif
