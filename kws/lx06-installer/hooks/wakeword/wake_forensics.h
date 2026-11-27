#ifndef WAKE_FORENSICS_H
#define WAKE_FORENSICS_H
#include <stddef.h>
#include <stdint.h>
int wake_mem_available_kib(const char *meminfo_path);
int wake_forensics_enabled(const char *flag_file);
int wake_forensics_keep(const char *keep_file);
int wake_sha256_file(const char *path, char out[65]);
/* json is a NEW event object. wav may be NULL. Pinned events count toward
 * both limits and are never automatically evicted or overwritten.
 * Returns 0 stored with audio, 1 stored without audio, 2 dropped for capacity
 * or an existing id (bounded .dropped counter), -1 on error. */
int wake_forensics_commit(const char *dir, const char *id, const char *json,
                          const void *wav, size_t wav_bytes,
                          int keep, int max_bytes, int max_pinned);
/* Writes a 16 kHz mono s16 WAV from pcm when allow_audio is set and the
 * memory floor was already checked by the caller. */
int wake_forensics_commit_pcm(const char *dir, const char *id, const char *json,
                              const int16_t *pcm, int samples, int allow_audio,
                              int keep, int max_bytes, int max_pinned);
/* Explicit user annotation of an existing event; retains its original WAV.
 * Same capacity rules; 2 refuses the update without incrementing drops. */
int wake_forensics_update_json(const char *dir, const char *id, const char *json,
                              int keep, int max_bytes, int max_pinned);
int wake_settings_read(const char *dir, const char *name, char *out, size_t cap);
int wake_settings_write(const char *dir, const char *name, const char *value);
int wake_id_ok(const char *id);
#endif
