#ifndef LX06_DOUBAO_ECHO_H
#define LX06_DOUBAO_ECHO_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define DB_ECHO_REFERENCE 3200u /* 800 ms at 4 kHz */
#define DB_ECHO_FRAME_BYTES 640u
#define DB_ECHO_LEAD_FRAMES 4u
#define DB_BARGE_DEFAULT_RMS 760u
#define DB_BARGE_MIN_RMS 400u
#define DB_BARGE_MAX_RMS 8000u
typedef struct {
    pthread_mutex_t lock;
    unsigned speech_rms;
    unsigned strong_protection;
    int16_t reference[DB_ECHO_REFERENCE];
    size_t head, used;
    unsigned phase;
    int sum;
    unsigned char partial, have_partial;
    uint64_t accepted_at_ms;
    unsigned char lead[DB_ECHO_LEAD_FRAMES][DB_ECHO_FRAME_BYTES];
    unsigned lead_index, lead_count, protected_frames, echo_frames, near_votes;
    uint64_t near_until_ms;
} db_echo;

void db_echo_reference(db_echo *, const unsigned char *, size_t bytes, uint64_t now);
int db_echo_keyword_filter(db_echo *, const unsigned char *pcm, uint64_t now,
                           unsigned char *clean);
int db_echo_filter(db_echo *, const unsigned char *pcm, int playing, uint64_t now,
                   unsigned char *uplink);

#endif
