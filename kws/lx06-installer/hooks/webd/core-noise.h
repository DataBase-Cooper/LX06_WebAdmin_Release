/* Web status samples only authenticated packet headers from the stock post-DSP
 * output. The kernel discards PCM. No persistent subscriber or recording. */
#include <sys/un.h>

#ifndef NOISE_MIC_DIR
#define NOISE_MIC_DIR "/tmp/lx06-mic"
#endif
#define NOISE_SOCKET NOISE_MIC_DIR "/webd-vpm-status.sock"
static struct {
  int fd, lock;
  pid_t pid;
  uint64_t birth;
  int64_t deadline, sampled, next, refresh_until;
  uint32_t seq;
  int first;
} noise_probe = { .fd = -1, .lock = -1 };

static void noise_close(void) {
  if (noise_probe.fd >= 0) {
    unlink(NOISE_SOCKET);
    close(noise_probe.fd);
    noise_probe.fd = -1;
  }
  if (noise_probe.lock >= 0) { close(noise_probe.lock); noise_probe.lock = -1; }
}

static void noise_start(pid_t pid, uint64_t birth, int64_t now);
static void noise_tick(int64_t now) {
  if (noise_probe.fd < 0 && noise_probe.pid > 0 && now < noise_probe.refresh_until &&
      now >= noise_probe.next)
    noise_start(noise_probe.pid, noise_probe.birth, now);
  if (noise_probe.fd < 0) return;
  for (int i = 0; i < 8; i++) {
    uint32_t head[4];
    struct iovec io = { head, sizeof(head) };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct ucred)) +
                                             CMSG_SPACE(sizeof(struct timespec))]; } control;
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = control.bytes,
                          .msg_controllen = sizeof(control.bytes) };
    ssize_t n = recvmsg(noise_probe.fd, &msg, MSG_TRUNC | MSG_DONTWAIT);
    if (n < 0) break;
    if (n != 336 || head[0] != 0x3141584c || head[2] != 16000 || head[3] != 320 ||
        (msg.msg_flags & MSG_CTRUNC)) continue;
    struct ucred cred = { .pid = -1 };
    struct timespec stamp = {0};
    struct timeval wall;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg,h)) {
      if (h->cmsg_level != SOL_SOCKET) continue;
      if (h->cmsg_type == SCM_CREDENTIALS && h->cmsg_len == CMSG_LEN(sizeof(cred)))
        memcpy(&cred, CMSG_DATA(h), sizeof(cred));
      if (h->cmsg_type == SCM_TIMESTAMPNS && h->cmsg_len == CMSG_LEN(sizeof(stamp)))
        memcpy(&stamp, CMSG_DATA(h), sizeof(stamp));
    }
    if (cred.uid != 0 || cred.pid != noise_probe.pid || !stamp.tv_sec || gettimeofday(&wall,NULL)) continue;
    int64_t age = ((int64_t)wall.tv_sec-stamp.tv_sec)*1000 + wall.tv_usec/1000 - stamp.tv_nsec/1000000;
    if (age < 0 || age > 900) { noise_probe.first = 0; continue; }
    if (noise_probe.first && head[1] == noise_probe.seq + 1) {
      noise_probe.sampled = now - age;
      noise_close();
      return;
    }
    noise_probe.first = 1;
    noise_probe.seq = head[1];
  }
  if (now >= noise_probe.deadline) noise_close();
}

static void noise_start(pid_t pid, uint64_t birth, int64_t now) {
  if (pid != noise_probe.pid || birth != noise_probe.birth) {
    noise_close(); noise_probe.sampled = noise_probe.next = noise_probe.refresh_until = 0;
    noise_probe.pid = pid; noise_probe.birth = birth;
  }
  if (!pid || !birth || noise_probe.fd >= 0 || now < noise_probe.next) return;
  noise_probe.next = now + 2000;
  if (mkdir(NOISE_MIC_DIR, 0755) && errno != EEXIST) return;
  int dir = open(NOISE_MIC_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  LX06_STATBUF st;
  if (dir < 0) return;
  if (LX06_FSTAT(dir, &st) || st.st_uid != 0 || (st.st_mode & 0022)) { close(dir); return; }
  int lock = openat(dir, "webd-vpm-status.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
    if (lock >= 0) close(lock);
    close(dir); return;
  }
  noise_probe.lock = lock;
  int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  int yes = 1, buf = 4096;
  struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = NOISE_SOCKET };
  if (fd >= 0 && fd < FD_SETSIZE &&
      !setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &yes, sizeof(yes)) &&
      !setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &yes, sizeof(yes))) {
    if (!LX06_STATAT(dir, "webd-vpm-status.sock", &st, AT_SYMLINK_NOFOLLOW) &&
        S_ISSOCK(st.st_mode) && st.st_uid == 0) unlinkat(dir, "webd-vpm-status.sock", 0);
    if (!bind(fd, (struct sockaddr *)&address, sizeof(address))) {
      setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
      noise_probe.fd = fd; noise_probe.first = 0; noise_probe.deadline = now + 900;
    }
  }
  close(dir);
  if (noise_probe.fd < 0) { if (fd >= 0) close(fd); noise_close(); }
}

/* When an inactive page is opened, report freshly sampled evidence in the
 * first response. A busy or silent source costs at most one 220ms probe in
 * two seconds and never opens an audio device or wakes an assistant. */
static void noise_prime(int64_t now) {
  static int64_t next;
  if (noise_probe.fd < 0 || now < next ||
      (noise_probe.sampled && now - noise_probe.sampled <= 6500)) return;
  next = now + 2000;
  int64_t end = now + 220;
  while (noise_probe.fd >= 0 && now < end) {
    struct pollfd p = {noise_probe.fd,POLLIN,0};
    if (poll(&p,1,(int)(end-now)) <= 0 || !(p.revents & POLLIN)) return;
    now = lk_now_ms();
    if (now < 0) return;
    noise_tick(now);
  }
}

static int noise_mapped(FILE *maps, const LX06_STATBUF *tap) {
  unsigned maj, min;
  /* /proc/maps uses Linux major:minor. glibc dev_t encodes the low eight
   * minor bits before the low twelve major bits, then their high portions. */
  uint64_t dev = tap->st_dev;
  unsigned disk_major = ((dev >> 8) & 0xfff) | ((dev >> 32) & 0xfffff000);
  unsigned disk_minor = (dev & 0xff) | ((dev >> 12) & 0xffffff00);
  unsigned long long ino;
  char line[768], perms[5];
  while (fgets(line, sizeof(line), maps)) {
    if (sscanf(line, "%*s %4s %*s %x:%x %llu", perms, &maj, &min, &ino) == 4 &&
        perms[2] == 'x' && ino && ino == (unsigned long long)tap->st_ino &&
        maj == disk_major && min == disk_minor) return 1;
  }
  return 0;
}

static int noise_capture(pid_t engine) {
  FILE *f = fopen("/proc/asound/card0/pcm3c/sub0/status", "r");
  int running = 0, owner = -1;
  char line[128], value[24];
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "state: %23s", value) == 1) running = !strcmp(value, "RUNNING");
    if (!strncmp(line, "owner_pid", 9)) (void)sscanf(line + 9, " : %d", &owner);
  }
  fclose(f);
  if (owner != engine && owner > 0) {
    /* ALSA can report the thread which opened the PCM. Check its thread group. */
    char task[80];
    snprintf(task, sizeof(task), "/proc/%ld/task/%d", (long)engine, owner);
    if (access(task, F_OK)) return 0;
  }
  return owner > 0 && running;
}
