#define _GNU_SOURCE
#include "official-player-lifetime.h"
#include "official-player-identity.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

/* Exact supported stock ABI: opaque C APIs and named PLT seams only. */
#define PLAYER_SLOTS 16
struct entry {
    const void *handle;
    struct lx06_player_lifetime value;
    unsigned releases;
    unsigned retirement_observed;
};
struct release_context {
    const void *handle;
    uint64_t generation;
    unsigned object_release_returned;
    struct release_context *previous;
};
static struct entry players[PLAYER_SLOTS];
static uint64_t next_generation;
static unsigned unknown;
static unsigned identity_verified;
static void *verified_library;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static __thread struct release_context *current_release;
static void *(*original_new)(void *);
static void (*original_retain)(void *);
static void (*original_release)(void *);
static void (*original_stop)(void *);
static void (*original_set_media)(void *, void *);
static int (*original_play)(void *);
static void (*original_object_release)(void *);
/* LX06 glibc 2.25 exports this GLIBC_2.4 registration ABI; its shipped
   libpthread has no dynamic pthread_atfork wrapper. Keep DSO ownership so
   unloading the observer also unregisters its callbacks. */
extern int __register_atfork(void (*)(void), void (*)(void), void (*)(void), void *);
extern void *__dso_handle;

static void before_fork(void) { pthread_mutex_lock(&mutex); }
static void after_fork_parent(void) { pthread_mutex_unlock(&mutex); }
static void after_fork_child(void) {
    /* The original event/input workers did not survive this fork. */
    unknown = 1;
    current_release = NULL;
    pthread_mutex_unlock(&mutex);
}
static void resolve(void) {
    original_new = (void *(*)(void *))dlsym(RTLD_NEXT, "libvlc_media_player_new");
    original_retain = (void (*)(void *))dlsym(RTLD_NEXT, "libvlc_media_player_retain");
    original_release = (void (*)(void *))dlsym(RTLD_NEXT, "libvlc_media_player_release");
    original_stop = (void (*)(void *))dlsym(RTLD_NEXT, "libvlc_media_player_stop");
    original_set_media = (void (*)(void *, void *))dlsym(RTLD_NEXT, "libvlc_media_player_set_media");
    original_play = (int (*)(void *))dlsym(RTLD_NEXT, "libvlc_media_player_play");
    original_object_release = (void (*)(void *))dlsym(RTLD_NEXT, "vlc_object_release");
    /* An incomplete interposer cannot forward the stock API. Do not silently
       drop a stop/release operation or fabricate a successful factory. */
    if (!original_new || !original_retain || !original_release || !original_stop ||
        !original_set_media || !original_play || !original_object_release) abort();
    /* Retire only the exact audited OEM image. An unknown implementation
       still receives every original API call, but supplies no owner tokens.
       The strong loader reference keeps the verified image mapped. */
    static const unsigned char expected[32] = {
        0xd8,0xdb,0x5a,0x37,0x21,0xad,0x07,0xa8,0x53,0x9c,0x35,0x28,0xcb,0xf1,0xb1,0xc4,
        0xe5,0x3d,0x9e,0x74,0x37,0x17,0x5a,0x69,0xcd,0x6b,0x5e,0xc8,0x7e,0x31,0x72,0xa3
    };
    const void *symbols[] = {(const void *)original_new, (const void *)original_retain,
        (const void *)original_release, (const void *)original_stop, (const void *)original_set_media,
        (const void *)original_play, (const void *)original_object_release};
    identity_verified = lx06_player_elf_exact(symbols, sizeof(symbols)/sizeof(symbols[0]),
                                               expected, 1086784u, &verified_library);
    if (!identity_verified) unknown = 1;
    if (__register_atfork(before_fork, after_fork_parent, after_fork_child, __dso_handle)) abort();
}
static struct entry *find_handle(const void *handle) {
    unsigned i;
    for (i = 0; i < PLAYER_SLOTS; i++)
        if (players[i].value.generation && players[i].handle == handle &&
            players[i].value.state != LX06_PLAYER_RETIRED) return &players[i];
    return NULL;
}
static struct entry *find_generation(uint64_t generation) {
    unsigned i;
    if (!generation) return NULL;
    for (i = 0; i < PLAYER_SLOTS; i++)
        if (players[i].value.generation == generation) return &players[i];
    return NULL;
}
static void uncertain(struct entry *entry) {
    unknown = 1;
    if (entry) entry->value.state = LX06_PLAYER_UNKNOWN;
}
enum operation { RETAIN, RELEASE, STOP, SET_MEDIA, PLAY };
static uint64_t begin(void *handle, enum operation operation) {
    struct entry *entry;
    uint64_t generation = 0;
    pthread_once(&once, resolve);
    if (!identity_verified) return 0;
    pthread_mutex_lock(&mutex);
    entry = find_handle(handle);
    if (!entry) {
        uncertain(NULL);
    } else {
        generation = entry->value.generation;
        if (entry->value.in_flight == UINT_MAX) abort();
        if (!entry->value.references) uncertain(entry);
        entry->value.in_flight++;
        if (operation == RETAIN) {
            if (entry->value.references == UINT_MAX) uncertain(entry);
            else entry->value.references++;
        } else if (operation == RELEASE) {
            if (entry->value.references) entry->value.references--;
            entry->releases++;
            if (entry->value.state != LX06_PLAYER_UNKNOWN)
                entry->value.state = LX06_PLAYER_RELEASING;
        } else if (operation == SET_MEDIA) {
            if (entry->value.media_generation == UINT64_MAX) uncertain(entry);
            else entry->value.media_generation++;
        }
    }
    pthread_mutex_unlock(&mutex);
    return generation;
}
static void finish(uint64_t generation, enum operation operation,
                   unsigned object_release_returned) {
    struct entry *entry;
    if (!identity_verified) return;
    pthread_mutex_lock(&mutex);
    entry = find_generation(generation);
    if (!entry || !entry->value.in_flight) {
        uncertain(entry);
    } else {
        entry->value.in_flight--;
        if (operation == RELEASE) {
            entry->releases--;
            if (object_release_returned) {
                /* The exact stock final-release path has already joined input
                   and event workers before this named object-release call.
                   Its outer release must also return before retirement. */
                entry->retirement_observed = 1;
                if (entry->value.references || entry->value.in_flight != entry->releases)
                    uncertain(entry);
            } else if (!entry->value.references && !entry->releases && !entry->retirement_observed) {
                /* A missed retain or different library cannot retire on the
                   shadow reference count alone. Preserve the discrepancy. */
                uncertain(entry);
            } else if (entry->value.state != LX06_PLAYER_UNKNOWN && !entry->releases) {
                entry->value.state = LX06_PLAYER_LIVE;
            }
        } else if (entry->value.state != LX06_PLAYER_UNKNOWN && !entry->releases) {
            entry->value.state = operation == STOP ? LX06_PLAYER_STOP_RETURNED : LX06_PLAYER_LIVE;
        }
        if (entry->retirement_observed && !entry->value.references && !entry->value.in_flight &&
            entry->value.state != LX06_PLAYER_UNKNOWN) entry->value.state = LX06_PLAYER_RETIRED;
    }
    pthread_mutex_unlock(&mutex);
}

void *libvlc_media_player_new(void *instance) {
    void *handle;
    struct entry *slot = NULL;
    unsigned i;
    pthread_once(&once, resolve);
    handle = original_new(instance);
    if (!handle || !identity_verified) return handle;
    pthread_mutex_lock(&mutex);
    slot = find_handle(handle);
    if (slot) {
        uncertain(slot);
    } else {
        for (i = 0; i < PLAYER_SLOTS; i++)
            if (!players[i].value.generation || players[i].value.state == LX06_PLAYER_RETIRED) {
                slot = &players[i]; break;
            }
        if (!slot || next_generation == UINT64_MAX) uncertain(NULL);
        else {
            *slot = (struct entry){0};
            slot->handle = handle;
            slot->value.generation = ++next_generation;
            slot->value.references = 1;
            slot->value.state = LX06_PLAYER_LIVE;
        }
    }
    pthread_mutex_unlock(&mutex);
    return handle;
}
void libvlc_media_player_retain(void *handle) {
    uint64_t generation = begin(handle, RETAIN);
    original_retain(handle);
    finish(generation, RETAIN, 0);
}
void libvlc_media_player_release(void *handle) {
    struct release_context context = {handle, begin(handle, RELEASE), 0, current_release};
    current_release = &context;
    original_release(handle);
    current_release = context.previous;
    finish(context.generation, RELEASE, context.object_release_returned);
}
void libvlc_media_player_stop(void *handle) {
    uint64_t generation = begin(handle, STOP);
    original_stop(handle);
    finish(generation, STOP, 0);
}
void libvlc_media_player_set_media(void *handle, void *media) {
    uint64_t generation = begin(handle, SET_MEDIA);
    original_set_media(handle, media);
    finish(generation, SET_MEDIA, 0);
}
int libvlc_media_player_play(void *handle) {
    uint64_t generation = begin(handle, PLAY);
    int result = original_play(handle);
    finish(generation, PLAY, 0);
    return result;
}
void vlc_object_release(void *object) {
    struct release_context *context;
    pthread_once(&once, resolve);
    original_object_release(object);
    for (context = current_release; context; context = context->previous)
        if (context->generation && context->handle == object) {
            context->object_release_returned = 1;
            break;
        }
}
int lx06_official_player_token(const void *handle, uint64_t *generation) {
    struct entry *entry;
    int found;
    if (!generation) return 0;
    *generation = 0;
    pthread_once(&once, resolve);
    if (!identity_verified) return 0;
    pthread_mutex_lock(&mutex);
    entry = find_handle(handle);
    found = entry != NULL;
    if (found) *generation = entry->value.generation;
    pthread_mutex_unlock(&mutex);
    return found;
}
int lx06_official_player_read(uint64_t generation, struct lx06_player_lifetime *out) {
    struct entry *entry;
    int found;
    if (!out) return 0;
    pthread_once(&once, resolve);
    if (!identity_verified) return 0;
    pthread_mutex_lock(&mutex);
    entry = find_generation(generation);
    found = entry != NULL;
    if (found) *out = entry->value;
    pthread_mutex_unlock(&mutex);
    return found;
}
void lx06_official_player_totals(struct lx06_player_lifetime_totals *out) {
    unsigned i;
    if (!out) return;
    pthread_once(&once, resolve);
    pthread_mutex_lock(&mutex);
    *out = (struct lx06_player_lifetime_totals){0, 0, unknown};
    for (i = 0; i < PLAYER_SLOTS; i++) {
        if (!players[i].value.generation) continue;
        out->in_flight += players[i].value.in_flight;
        if (players[i].value.state != LX06_PLAYER_RETIRED) out->occupied++;
    }
    pthread_mutex_unlock(&mutex);
}
