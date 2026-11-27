/* P0.5 local-wake arbitration constants. See the v2 revision-4 plan §5.4. */
#ifndef WAKE_POLICY_H
#define WAKE_POLICY_H
#define WW_CALIBRATION_POLICY "duration-bank2-candidate2-norm"
#define WAKE_NORM_BAND 0.15f          /* B: competitor band above a word's own gate */
#define WAKE_NORM_MARGIN 0.10f        /* M: minimum normalized gap */
#define WAKE_GRAY_MARGIN 0.25f        /* cloud verify only inside this normalized gap */
#define WAKE_GATE_CEILING 0.26       /* maximum stored and automatically calibrated gate */
#define WAKE_AUTO_GATE_CEILING WAKE_GATE_CEILING
/* Wider eligibility is only for a cloud-confirmed, physically paired word.
 * The per-word competitor band still applies before and after recognition. */
#define WAKE_CLOUD_NEAR_CEILING 0.295f
#define WAKE_LOCAL_NEAR_CEILING 0.25f
#define WAKE_LOCAL_SEPARATED_CEILING 0.265f /* bounded runtime-only candidate */
#define WAKE_LOCAL_SEPARATED_MARGIN 0.025f  /* raw distance to every observed rival */
#define WAKE_LOCAL_STRONG_SEPARATION_RATIO 1.22f
#define WAKE_LOCAL_STRONG_SEPARATION_CEILING .26f
#define WAKE_LOCAL_STRONG_SEPARATION_MARGIN .04f
#define WAKE_ENROLL_MIN 3
#define WAKE_ENROLL_MAX 5
#define WAKE_ENROLL_LEGACY_MAX 10     /* existing user recordings are retained */
#define WAKE_CLOUD_MAX_SAMPLES 28800  /* 1.8s at 16 kHz; longer clips are not verified */
#define WAKE_CLOUD_BUDGET_MS 800      /* firmware constant, includes encode time */
#define WAKE_CLOUD_IDLE_SEC 10        /* official-idle window before a verify send */
#define WAKE_PINYIN_WARN 0.60f        /* text pronunciation warning, not an acoustic gate */
#define WAKE_FORENSICS_KEEP_DEFAULT 20
#define WAKE_FORENSICS_KEEP_MIN 5
#define WAKE_FORENSICS_KEEP_MAX 30
#define WAKE_FORENSICS_MAX_PINNED 10
#define WAKE_FORENSICS_MAX_BYTES (4 * 1024 * 1024)
#define WAKE_FORENSICS_MEM_FLOOR_KIB (16 * 1024)
#define WAKE_FORENSICS_DIR "/tmp/lx06-wake-forensics"
#define WAKE_DATA_DIR "/data/wakeword"
#define WAKE_NATIVE_EVENT "/tmp/lx06-native-wake"
#define WAKE_ROUTER_STATE "/tmp/lx06-official-router.state"
#define WAKE_INSTRUCTION_LOG "/tmp/mico_aivs_lab/instruction.log"
#define WAKE_SESSION_FILE "/tmp/lx06-session.owner"
#define WAKE_CLOUD_HELPER "/usr/lib/lx06/wakeword/wake_cloud_asr"
#define WAKE_CLOUD_SOCKET "/tmp/lx06-control-asr-probe.sock"

/* Joint calibration may only tighten. Returns 1 when a cross recording still
 * falls inside this word's gate after the tightest gate that keeps every
 * held-out recording. *out is never greater than base. */
static int wake_joint_tighten(float base, float ceiling, float band,
                              const float *heldout, int nheld,
                              const float *cross, int ncross,
                              float *out, int *near_out) {
    float threshold = base < ceiling ? base : ceiling, min_cross = 99.0f, max_hold = 0.0f;
    int i, near = 0, conflict = 0;
    if (out) *out = base;
    if (near_out) *near_out = 0;
    if (!(base > 0.0f) || !(ceiling > 0.0f) || !(band > 0.0f)) {
        if (out) *out = base;
        if (near_out) *near_out = 0;
        return 1;
    }
    if (!heldout || nheld < 1 || ncross < 0 || (ncross && !cross)) return 1;
    for (i = 0; i < nheld; i++) {
        if (!(heldout[i] >= 0.0f && heldout[i] < 99.0f)) { if (out) *out = base; return 1; }
        if (heldout[i] > max_hold) max_hold = heldout[i];
    }
    for (i = 0; i < ncross; i++) {
        float ratio;
        if (!(cross[i] >= 0.0f && cross[i] <= 99.0f)) { if (out) *out = base; return 1; }
        if (cross[i] == 99.0f) continue;
        if (cross[i] < min_cross) min_cross = cross[i];
        ratio = cross[i] / base;
        if (ratio < 1.0f) conflict = 1;
        else if (ratio < 1.0f + band) near++;
    }
    if (ncross && min_cross < 99.0f && (conflict || near)) {
        float tightened = min_cross / (1.0f + band);
        if (tightened > 0.0f && tightened < threshold && max_hold <= tightened && tightened <= ceiling)
            threshold = tightened;
    }
    near = 0;
    conflict = max_hold > threshold;
    for (i = 0; i < ncross; i++) {
        float ratio;
        if (!(cross[i] >= 0.0f && cross[i] <= 99.0f)) { if (out) *out = base; return 1; }
        if (cross[i] == 99.0f) continue;
        ratio = cross[i] / threshold;
        if (ratio < 1.0f) conflict = 1;
        else if (ratio < 1.0f + band) near++;
    }
    if (threshold > base) threshold = base;
    if (out) *out = threshold;
    if (near_out) *near_out = near;
    return conflict;
}
#endif
