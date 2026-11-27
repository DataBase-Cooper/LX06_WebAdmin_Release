#ifndef LX06_OTA_STREAM_H
#define LX06_OTA_STREAM_H
#include <stddef.h>
/* Read-only integrity verification. No flash authorization or filesystem I/O. */
typedef struct {
    char version[64];
    size_t kernel_size, rootfs_size;
    char kernel_hash[65], rootfs_hash[65], payload_hash[65];
} ota_manifest;
typedef struct ota_stream ota_stream;
ota_stream *ota_stream_new(void);
void ota_stream_free(ota_stream *s);
size_t ota_stream_storage(void);
int ota_stream_feed(ota_stream *s, const void *data, size_t size);
int ota_stream_finish(ota_stream *s);
/* Only available after explicit EOF and successful complete verification. */
const ota_manifest *ota_stream_manifest(const ota_stream *s);
const char *ota_stream_error(const ota_stream *s);
#endif
