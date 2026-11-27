#ifndef XZ_IDENTITY_H
#define XZ_IDENTITY_H

#include <stddef.h>

/* Read a syntactically valid persisted UUID, or atomically create a UUIDv4. */
int xz_client_id_load_or_create(const char *path, char *out, size_t capacity);

#endif
