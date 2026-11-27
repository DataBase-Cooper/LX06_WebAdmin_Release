#ifndef LX06_WEB_WAKE_PROFILE_H
#define LX06_WEB_WAKE_PROFILE_H

#ifndef WW_EPOCH_PROFILE
#define WW_EPOCH_PROFILE WWHOME "/epoch-capture-v2"
#endif
#define WW_RAW_DOMAIN "raw-slot4-hp65-g8-kwe2-v2"
#define WW_RAW_STORE WWBASE "/profiles/" WW_RAW_DOMAIN
#define WW_JOINT_ROOT (ww_epoch_capture_enabled() ? "/tmp/lx06-wake-joint-raw-hp65-g8-kwe2-v2" : "/tmp/lx06-wake-joint")
#define WW_VALIDATION_ROOT (ww_epoch_capture_enabled() ? "/tmp/lx06-wake-validation-samples-raw-hp65-g8-kwe2-v2" : "/tmp/lx06-wake-validation-samples")
#define WW_RAW_FORENSICS "/tmp/lx06-wake-forensics-raw-hp65-g8-kwe2-v2"

static int ww_epoch_capture_enabled(void) {
#ifdef LX06_PRIVATE_RAW_PROFILE
  return access(WW_EPOCH_PROFILE, R_OK) == 0;
#else
  /* An experimental capture marker must never replace the user's stock
   * post-DSP recordings, calibrated models, bindings or primary detector. */
  return 0;
#endif
}

/* Never adopt an existing unmarked directory: it may hold another acoustic
 * domain. Publish the identity only in a new directory under the mutation lock. */
static int ww_raw_store_ready(int create) {
  const int directory = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  int root = -1, profiles = -1, raw = -1, marker = -1, ready = 0;
  if (create && mkdir(WWBASE, 0755) && errno != EEXIST) goto done;
  if ((root = open(WWBASE, directory)) < 0) goto done;
  if (create && mkdirat(root, "profiles", 0755) && errno != EEXIST) goto done;
  if ((profiles = openat(root, "profiles", directory)) < 0) goto done;
  int fresh = create && !mkdirat(profiles, WW_RAW_DOMAIN, 0755);
  if ((raw = openat(profiles, WW_RAW_DOMAIN, directory)) < 0) goto done;
  if (fresh) {
    marker = openat(raw, "capture-domain", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (marker < 0) {
      unlinkat(profiles, WW_RAW_DOMAIN, AT_REMOVEDIR);
      goto done;
    }
    int failed = write_all(marker, WW_RAW_DOMAIN "\n", sizeof(WW_RAW_DOMAIN)) || fsync(marker);
    if (close(marker)) failed = 1;
    marker = -1;
    if (failed) {
      unlinkat(raw, "capture-domain", 0);
      unlinkat(profiles, WW_RAW_DOMAIN, AT_REMOVEDIR);
      goto done;
    }
  }
  marker = openat(raw, "capture-domain", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  char identity[sizeof(WW_RAW_DOMAIN) + 1];
  if (marker >= 0) {
    ssize_t n = read(marker, identity, sizeof(identity));
    ready = (n == sizeof(WW_RAW_DOMAIN) - 1 || (n == sizeof(WW_RAW_DOMAIN) && identity[n-1] == '\n')) &&
            !memcmp(identity, WW_RAW_DOMAIN, sizeof(WW_RAW_DOMAIN)-1);
  }
done:
  if (marker >= 0) close(marker);
  if (raw >= 0) close(raw);
  if (profiles >= 0) close(profiles);
  if (root >= 0) close(root);
  return ready;
}

/* An invalid selected profile cannot fall back to DNN models or accept writes. */
#define WW_STORE_PATH(suffix) (!ww_epoch_capture_enabled() ? WWBASE suffix : \
                               ww_raw_store_ready(0) ? WW_RAW_STORE suffix : "/dev/null" suffix)
#endif
