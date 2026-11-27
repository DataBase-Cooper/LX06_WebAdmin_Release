/* Initialize only the stock, unused RAM-backed swap device. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define ZRAM "/sys/block/zram0/"
#define DEVICE "/dev/zram0"
#define SWAP_BYTES (24u * 1024u * 1024u)
#define RAM_LIMIT "8388608\n"

static int set_value(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t bytes = strlen(value);
    int ok = write(fd, value, bytes) == (ssize_t)bytes;
    close(fd);
    return ok ? 0 : -1;
}

static int read_value(const char *path, unsigned long *value) {
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int ok = fscanf(file, "%lu", value) == 1;
    fclose(file);
    return ok ? 0 : -1;
}

int main(void) {
    int lock = open("/tmp/lx06-zram.lock", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 1;
    unsigned int dev_major, dev_minor;
    FILE *sysdev = fopen(ZRAM "dev", "r");
    if (!sysdev) return 1;
    int device_ok = fscanf(sysdev, "%u:%u", &dev_major, &dev_minor) == 2;
    fclose(sysdev);
    struct stat st;
    if (!device_ok || lstat(DEVICE, &st) || !S_ISBLK(st.st_mode) ||
        st.st_rdev != makedev(dev_major, dev_minor)) return 1;

    unsigned long size, limit;
    if (read_value(ZRAM "disksize", &size) || read_value(ZRAM "mem_limit", &limit)) return 1;
    if (size != 0) return 0;

    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096 || page > 65536 || SWAP_BYTES % page) return 1;
    unsigned char *header = calloc(1, (size_t)page);
    if (!header) return 1;
    /* Linux swap v1: native-endian version and last page at offset 1024. */
    uint32_t fields[] = { 1, SWAP_BYTES / (uint32_t)page - 1, 0 };
    memcpy(header + 1024, fields, sizeof fields);
    memcpy(header + page - 10, "SWAPSPACE2", 10);
    char capacity[32];
    snprintf(capacity, sizeof capacity, "%u\n", SWAP_BYTES);
    if (set_value(ZRAM "disksize", capacity)) { free(header); return 1; }

    int ready = 0;
    if (!set_value(ZRAM "mem_limit", RAM_LIMIT) &&
        !read_value(ZRAM "mem_limit", &limit) && limit == 8388608ul) {
        int fd = open(DEVICE, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            ready = !fstat(fd, &st) && S_ISBLK(st.st_mode) &&
                st.st_rdev == makedev(dev_major, dev_minor) &&
                write(fd, header, (size_t)page) == page && !fsync(fd);
            close(fd);
        }
        if (ready) ready = swapon(DEVICE, 0) == 0;
    }
    free(header);
    if (!ready) {
        (void)set_value(ZRAM "reset", "1\n");
        fputs("lx06-zram: compressed swap unavailable\n", stderr);
        return 1;
    }
    return 0;
}
