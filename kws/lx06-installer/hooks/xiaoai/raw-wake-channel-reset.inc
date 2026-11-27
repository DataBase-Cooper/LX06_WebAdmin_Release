/* Included only for the verified PDM channel-reset capture route. */
static int raw_same_trigger(const struct raw_alsa_status *a, const struct raw_alsa_status *b);
static int raw_valid_format(void *pcm);

static struct {
    int (*drop)(void *);
    int (*hw_free)(void *);
    int (*hw_params_any)(void *, void *);
    int (*hw_params_set_channels)(void *, void *, unsigned);
    int (*hw_params_set_rate)(void *, void *, unsigned, int);
    int (*hw_params_set_format)(void *, void *, int);
    int (*hw_params_set_access)(void *, void *, int);
    int (*hw_params_set_period_size)(void *, void *, unsigned long, int);
    int (*hw_params_set_buffer_size)(void *, void *, unsigned long);
    int (*hw_params)(void *, void *);
    size_t (*sw_params_sizeof)(void);
    int (*sw_params_current)(void *, void *);
    int (*sw_params)(void *, void *);
    int (*prepare)(void *);
    int (*start)(void *);
} reset_alsa;

static int raw_reset_resolve(void) {
#define LOAD(name) do { \
    reset_alsa.name = dlsym(RTLD_NEXT, "snd_pcm_" #name); \
    if (!reset_alsa.name) return 0; \
} while (0)
    LOAD(drop); LOAD(hw_free); LOAD(hw_params_any); LOAD(hw_params_set_channels);
    LOAD(hw_params_set_rate); LOAD(hw_params_set_format); LOAD(hw_params_set_access);
    LOAD(hw_params_set_period_size); LOAD(hw_params_set_buffer_size); LOAD(hw_params);
    LOAD(sw_params_sizeof); LOAD(sw_params_current); LOAD(sw_params); LOAD(prepare); LOAD(start);
#undef LOAD
    return 1;
}

static struct {
    void *pcm;
    unsigned char idle[16];
    struct raw_alsa_status trigger;
    int qualified;
} raw_reset;

static int raw_prepare_channels(void *pcm, unsigned channels) {
    union raw_params_storage params;
    unsigned actual;
    /* ALSA can report a positive refinement count on success. */
    return reset_alsa.hw_params_any(pcm, &params) >= 0 &&
        reset_alsa.hw_params_set_access(pcm, &params, 3) == 0 &&
        reset_alsa.hw_params_set_format(pcm, &params, 2) == 0 &&
        reset_alsa.hw_params_set_rate(pcm, &params, 48000, 0) == 0 &&
        reset_alsa.hw_params_set_channels(pcm, &params, channels) == 0 &&
        reset_alsa.hw_params_set_period_size(pcm, &params, RAW_CAPTURE_PERIOD, 0) == 0 &&
        reset_alsa.hw_params_set_buffer_size(pcm, &params, RAW_CAPTURE_BUFFER) == 0 &&
        reset_alsa.hw_params(pcm, &params) == 0 && reset_alsa.prepare(pcm) == 0 &&
        raw_params_current(pcm, &params) == 0 && raw_params_channels(&params, &actual) == 0 && actual == channels;
}

static int raw_reset_epoch(void *pcm, const unsigned char idle[16], struct raw_alsa_status *before) {
    if (raw_reset.pcm == pcm && !memcmp(idle, raw_reset.idle, 16) &&
        (!raw_reset.qualified || raw_same_trigger(&raw_reset.trigger, before))) return raw_reset.qualified;
    int lease = lx06_idle_epoch_take(idle);
    if (lease < 0) return 0;
    /* One attempt per idle epoch. A failed proof must never issue old audio. */
    memcpy(raw_reset.idle, idle, 16);
    raw_reset.pcm = pcm;
    raw_reset.qualified = 0;
    union raw_params_storage original_hw = {0}, original_sw;
    size_t sw_size = reset_alsa.sw_params_sizeof();
    if (!sw_size || sw_size > sizeof(original_sw) || !raw_valid_format(pcm) ||
        raw_params_current(pcm, &original_hw) != 0 || reset_alsa.sw_params_current(pcm, &original_sw) != 0 ||
        reset_alsa.drop(pcm) != 0) {
        close(lease);
        return 0;
    }
    /* A113X PDM_CTRL[15:8] asserts reset for every unused channel. The
     * verified two-channel prepare resets the out-of-mask slot 4. */
    int narrowed = reset_alsa.hw_free(pcm) == 0 && raw_prepare_channels(pcm, 2);
    (void)reset_alsa.hw_free(pcm);
    /* Some drivers reject reapplying the read-only current-params snapshot.
     * Preserve it for exact comparison with the freshly applied settings. */
    union raw_params_storage applied = original_hw;
    int hw = reset_alsa.hw_params(pcm, &applied);
    if (hw == 0 || (reset_alsa.hw_free(pcm) == 0 && raw_prepare_channels(pcm, 8))) {
        memset(&applied, 0, sizeof(applied));
        int exact = raw_params_current(pcm, &applied) == 0 &&
                    !memcmp(&original_hw, &applied, raw_params_size());
        int sw = reset_alsa.sw_params(pcm, &original_sw);
        int ready = reset_alsa.prepare(pcm);
        int started = ready == 0 ? reset_alsa.start(pcm) : ready;
        if (exact && narrowed && sw == 0 && started == 0 && raw_valid_format(pcm) &&
            raw_status(pcm, &raw_reset.trigger) == 0 && !raw_same_trigger(before, &raw_reset.trigger)) {
            raw_reset.qualified = 1;
            *before = raw_reset.trigger;
        }
    }
    close(lease);
    return raw_reset.qualified;
}
