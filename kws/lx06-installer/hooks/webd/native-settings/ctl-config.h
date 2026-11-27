/* T21 isolated native draft. No production include/build registration. */
#ifndef LX06_CTL_CONFIG_DRAFT_H
#define LX06_CTL_CONFIG_DRAFT_H
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T10/T14 pending: this settings draft is fixture-only"
#endif
#include <stddef.h>
#include <stdint.h>

/* Draft resource ceilings, not newly approved product/wire limits. */
#define CFG_BYTES 8192
/* Private declaration analysis budget, separate from CFG_BYTES records. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision11 declaration capacity is fixture-only"
#endif
#define CFG_DECL_BYTES 65536
#define CFG_CANDIDATES 1024
#define CFG_ENTRIES 512
#define CFG_APPS 16
#define CFG_ARGS 32
enum cfg_status { CFG_OK=0, CFG_INVALID=-1, CFG_CAPACITY=-2, CFG_PENDING_FLOW=-3 };
struct cfg_entry { char key[81]; uint32_t offset; unsigned char overridden; };
struct cfg {
    char arena[CFG_DECL_BYTES+1];
    struct cfg_entry entries[CFG_ENTRIES];
    size_t used, count;
    char apps[CFG_APPS][33];
    size_t app_count;
};
struct cfg_override {
    const char *key;
    /* CTL uses the whole ordered list, separated by LF; "" disables it.
     * Restore defaults by omitting the override. No persistence schema here. */
    const char *value;
};
struct cfg_arg { char key[81], ref[17]; size_t offset, length; };
struct cfg_row {
    char op[81];
    char refs[4][17];
    size_t ref_count, arg_count, phrase_count;
    struct cfg_arg args[CFG_ARGS];
};
struct cfg_control {
    unsigned start_ms, max_ms;
    int enabled, end_on_timeout;
    const char *bargein, *exit, *hangup;
};
/* Atomic in-memory outputs on error. Input is bounded header text, not the
 * full script. Stop at first executable line; never execute/source metadata.
 * Opaque transport/audio/SET/workflow strings are declarations only. B owns
 * caps, runtime operation validation, and connection policy. */
int cfg_parse(const char *header, size_t length, struct cfg *out);
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision11 upload parser extension is fixture-only"
#endif
/* Complete normalized immutable upload. The original parser loop stops at
 * first executable line. Only complete leading blank/comment lines count
 * toward CFG_DECL_BYTES and are checked as UTF-8. Body is opaque. No execute.
 * Atomic cfg AND extent on error; header-only cfg_parse behavior unchanged. */
int cfg_parse_script(const char *script, size_t length, struct cfg *out, size_t *extent);
/* TOKEN is private declaration input only; generic overrides reject it.
 * Explicit secret save/sync belongs to the later T22 lifecycle consumer. */
int cfg_effective(const struct cfg *defaults, const struct cfg_override *overrides,
                  size_t count, struct cfg *out);
int cfg_parse_row(const char *raw, struct cfg_row *out);
/* Shared bounded UTF-8 check for these two draft translation units. */
int cfg_utf8_valid(const char *text, size_t length);
const char *cfg_value(const struct cfg *c, const char *key);
const char *cfg_ctl_row(const struct cfg *c, const char *app, size_t index);
int cfg_control_view(const struct cfg *c, const char *app, struct cfg_control *out);
void cfg_channel_timing(const struct cfg *c, unsigned *start_ms, unsigned *max_ms);
int cfg_require_exit(const struct cfg *c, const char *app, int voice);

/* Typed declaration view, never runtime capability validation. Caller owns c
 * and must retain it unchanged for the lifetime of the returned pointers.
 * Session kind comes from the owner/caps; no App-name or audio-based guessing.
 * SET/workflow/ring/path/capability checks remain with their existing owners.
 * No token, host, full header, or user settings record in this public view. */
enum cfg_session_kind { CFG_SESSION_SILENT=0, CFG_SESSION_VOICE, CFG_SESSION_COMMAND };
enum cfg_audio_bit { CFG_AUDIO_MIC=1, CFG_AUDIO_SPEAKER=2, CFG_AUDIO_RING=4 };
struct cfg_bridge_app {
    const char *app;
    struct cfg_control control;
    unsigned audio_present, audio_open;
    uint32_t connect_timeout_ms;
    int connect_timeout_present;
    const char *ring_tone, *workflow, *set, *conn, *play_tpl;
    const char *mute, *unmute, *muteout;
};
/* Atomic output: any failure leaves *out byte-identical. Missing audio fields
 * are unspecified, not forced closed. Missing connect timeout is delegated to
 * caps; channel/CTL window defaults must never become connection defaults. */
int cfg_bridge_app_view(const struct cfg *c, const char *app,
                        enum cfg_session_kind session, struct cfg_bridge_app *out);
/* Enumerate canonical identities from the SAME effective native object.
 * Borrowed pointer; NULL at/outside the end. No default-first-App selection. */
const char *cfg_bridge_app_name(const struct cfg *c, size_t index);

/* Single parsed object -> top-level declaration, never permission to dial.
 * UNKNOWN retains the existing launcher's fallback; it must not enable a
 * bridge. App-only partial headers remain offline declarations (UNSPECIFIED).
 * No raw endpoint, token, label or unknown mode is returned. */
enum cfg_mode { CFG_MODE_UNSPECIFIED=0, CFG_MODE_CLOUD_E2E, CFG_MODE_ACCOUNT_TEXT,
                CFG_MODE_PHONE_BRIDGE, CFG_MODE_ACTION, CFG_MODE_UNKNOWN };
enum cfg_field_state { CFG_FIELD_ABSENT=0, CFG_FIELD_EMPTY,
                       CFG_FIELD_PLACEHOLDER, CFG_FIELD_VALUE };
struct cfg_metadata {
    enum cfg_mode mode;
    int bridge_enabled, protocol_declared;
    size_t app_count;
    int local_wakeup, official_wakeup, local_window, official_channel;
    unsigned start_ms, max_ms;
    enum cfg_field_state host_state, port_state, token_state;
    unsigned port;
};
/* Explicit mode/alias conflicts, wrong bridge protocol and invalid numeric
 * ports fail atomically. Missing/empty/placeholders are incomplete input, not
 * defaults or connection credentials. K0/K3 still own runtime validation. */
int cfg_metadata_view(const struct cfg *c, struct cfg_metadata *out);

/* Adapter to the EXISTING checked settings directory/.lock/<script>.json.
 * The owner callback merges a FULL existing flat-string JSON record, including
 * unrelated fields/passwords, under the lock. It also owns field validation.
 * No new disk namespace or CTL serialization layout is declared here.
 * Return: 0 committed+durable; 1 renamed but directory fsync failed (uncertain
 * durability); -1 refused before rename. Never prints raw/private input. */
typedef int (*cfg_record_edit)(const char *current, char next[CFG_BYTES+1], void *ctx);
int cfg_store_open(int create);
/* Same checked read as transaction: 1 present, 0 missing, -1 untrusted.
 * A missing file is returned as "{}". No lock is acquired for a snapshot. */
int cfg_store_read(int checked_dir, const char *script, char record[CFG_BYTES+1]);
/* Original helper already owns .lock while loading schema/password values.
 * Validate and reuse that same lock; caller retains and closes its descriptor. */
int cfg_store_transaction_locked(int checked_dir, int lock, const char *script,
                                 cfg_record_edit edit, void *ctx);
/* Fixture-local owner transaction: recheck dependencies after temp fsync and
 * immediately before rename (also on no-op). Caller coordinates every script
 * upload/delete and settings write using the SAME .lock. A guard alone cannot
 * serialize an uncooperative writer. No new production ABI is registered. */
typedef int (*cfg_record_guard)(void *ctx);
int cfg_store_guarded_locked(int checked_dir, int lock, const char *script,
                            cfg_record_edit edit, cfg_record_guard guard, void *ctx);
int cfg_store_transaction(int checked_dir, const char *script, cfg_record_edit edit, void *ctx);
#endif
