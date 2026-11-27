#define _GNU_SOURCE
#include "official-player-identity.h"
#include "schedule-hash.h"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* All IO is once at observer initialization, outside its player mutex. This
 * is an ABI guard for trusted installed files, not protection against code
 * modifying its own address space after verification. */
#define MAX_IMAGE_BYTES (16u * 1024u * 1024u)
#define MAX_PROGRAM_HEADERS 128u
#if defined(__arm__) && defined(__GLIBC__)
typedef struct stat64 image_stat;
extern int __fxstat64(int, int, struct stat64 *);
static int image_fstat(int fd, image_stat *st) { return __fxstat64(3, fd, st); }
#else
typedef struct stat image_stat;
static int image_fstat(int fd, image_stat *st) { return fstat(fd, st); }
#endif
struct comparison {
    uintptr_t base;
    int fd, found, valid;
    size_t length, count;
    const void *const *symbols;
    unsigned char buffer[1024];
};
static int same_stat(const image_stat *a, const image_stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_size == b->st_size && a->st_mode == b->st_mode &&
        a->st_uid == b->st_uid && a->st_gid == b->st_gid &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int read_exact(int fd, void *out, size_t count, size_t offset) {
    unsigned char *bytes = out;
    while (count) {
        ssize_t got = pread(fd, bytes, count, (off_t)offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return 0;
        bytes += got; count -= (size_t)got; offset += (size_t)got;
    }
    return 1;
}
static int compare_loaded(struct dl_phdr_info *info, size_t size, void *opaque) {
    struct comparison *c = opaque;
    ElfW(Ehdr) header;
    unsigned i, executable = 0;
    (void)size;
    if ((uintptr_t)info->dlpi_addr != c->base) return 0;
    c->found++;
    if (c->found != 1 || !read_exact(c->fd, &header, sizeof(header), 0) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_type != ET_DYN ||
        header.e_ehsize != sizeof(header) || header.e_phentsize != sizeof(ElfW(Phdr)) ||
        !header.e_phnum || header.e_phnum > MAX_PROGRAM_HEADERS ||
        header.e_phnum != info->dlpi_phnum || header.e_phoff > c->length ||
        (size_t)header.e_phnum * sizeof(ElfW(Phdr)) > c->length - header.e_phoff) return 1;
    for (i = 0; i < info->dlpi_phnum; i++) {
        ElfW(Phdr) p;
        if (!read_exact(c->fd, &p, sizeof(p), header.e_phoff + i * sizeof(p)) ||
            memcmp(&p, info->dlpi_phdr + i, sizeof(p))) return 1;
        if (p.p_type != PT_LOAD && p.p_type != PT_DYNAMIC) continue;
        if (p.p_offset > c->length || p.p_filesz > c->length - p.p_offset ||
            p.p_memsz < p.p_filesz || p.p_vaddr > UINTPTR_MAX - c->base ||
            p.p_memsz > UINTPTR_MAX - c->base - p.p_vaddr) return 1;
        if (p.p_type == PT_DYNAMIC) {
            size_t offset;
            int terminated = 0;
            for (offset = 0; offset + sizeof(ElfW(Dyn)) <= p.p_filesz; offset += sizeof(ElfW(Dyn))) {
                ElfW(Dyn) item;
                if (!read_exact(c->fd, &item, sizeof(item), p.p_offset + offset)) return 1;
                if (item.d_tag == DT_TEXTREL ||
                    (item.d_tag == DT_FLAGS && (item.d_un.d_val & DF_TEXTREL))) return 1;
                if (item.d_tag == DT_NULL) { terminated = 1; break; }
            }
            if (!terminated) return 1;
            continue;
        }
        if (p.p_flags & PF_X) {
            if (p.p_flags & PF_W) return 1;
            executable++;
        }
        if (!(p.p_flags & PF_W)) {
            size_t offset;
            if (!(p.p_flags & PF_R)) return 1;
            for (offset = 0; offset < p.p_filesz;) {
                size_t amount = p.p_filesz - offset;
                if (amount > sizeof(c->buffer)) amount = sizeof(c->buffer);
                if (!read_exact(c->fd, c->buffer, amount, p.p_offset + offset) ||
                    memcmp(c->buffer, (const void *)(c->base + p.p_vaddr + offset), amount)) return 1;
                offset += amount;
            }
        }
    }
    if (!executable) return 1;
    for (size_t symbol = 0; symbol < c->count; symbol++) {
        uintptr_t address = (uintptr_t)c->symbols[symbol];
        int covered = 0;
#if defined(__arm__)
        address &= ~(uintptr_t)1; /* The public ARM function-pointer Thumb bit. */
#endif
        for (i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *p = info->dlpi_phdr + i;
            uintptr_t start = c->base + p->p_vaddr;
            if (p->p_type == PT_LOAD && (p->p_flags & PF_X) &&
                address >= start && address - start < p->p_filesz) covered = 1;
        }
        if (!covered) return 1;
    }
    c->valid = 1;
    return 1;
}
int lx06_player_elf_exact(const void *const *symbols, size_t count,
                          const unsigned char expected[32], size_t length, void **pin) {
    Dl_info first, current;
    struct link_map *map = NULL;
    struct comparison c;
    image_stat before, after;
    unsigned char digest[32];
    void *handle = NULL;
    int fd = -1, good = 0;
    if (pin) *pin = NULL;
    if (!pin || !symbols || !count || count > 32 || !expected ||
        length < sizeof(ElfW(Ehdr)) || length > MAX_IMAGE_BYTES ||
        !dladdr(symbols[0], &first) || !first.dli_fbase || !first.dli_fname ||
        first.dli_fname[0] != '/') return 0;
    for (size_t i = 0; i < count; i++)
        if (!symbols[i] || !dladdr(symbols[i], &current) ||
            current.dli_fbase != first.dli_fbase) return 0;
    handle = dlopen(first.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!handle || dlinfo(handle, RTLD_DI_LINKMAP, &map) || !map ||
        (uintptr_t)map->l_addr != (uintptr_t)first.dli_fbase) goto done;
    fd = open(first.dli_fname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || image_fstat(fd, &before) || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || (uint64_t)before.st_size != length ||
        (before.st_mode & 0022)) goto done;
    memset(&c, 0, sizeof(c));
    c.base = (uintptr_t)first.dli_fbase; c.fd = fd; c.length = length;
    c.count = count; c.symbols = symbols;
    if (!eo_schedule_hash_fd(fd, length, c.buffer, sizeof(c.buffer), digest) ||
        memcmp(digest, expected, sizeof(digest))) goto done;
    dl_iterate_phdr(compare_loaded, &c);
    if (c.found != 1 || !c.valid || image_fstat(fd, &after) || !same_stat(&before, &after)) goto done;
    *pin = handle; handle = NULL; good = 1;
done:
    if (fd >= 0) close(fd);
    if (handle) dlclose(handle);
    return good;
}
