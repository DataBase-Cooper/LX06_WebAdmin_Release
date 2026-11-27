/* Private host experiment. No persistent schema, public ABI or permission. */
#ifndef LX06_RECOGNITION_POLICY_DRAFT_H
#define LX06_RECOGNITION_POLICY_DRAFT_H
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition policy is fixture-only; production integration pending"
#endif
#include "declaration-expression.h"
enum rec_mode { REC_OFF=0, REC_REGEX, REC_MODEL };
enum rec_dispatch { REC_DIRECT=0, REC_LISTEN };
enum { REC_FIXED_PATH=1, REC_REGEX_PATH=2, REC_MODEL_PATH=4 };
struct rec_channel {
    int mode, wake_dispatch;
    /* Borrowed resolved values, NOT credentials or independent regex tables.
     * prompt is resolved by the owner (channel override -> connection).
     * NULL/ASCII whitespace means empty. Off retains but ignores both. */
    const char *connection_id, *prompt;
};
struct rec_settings {
    int fixed_enabled, regex_enabled, model_enabled, channels_present;
    struct rec_channel official, local;
};
struct rec_effective {
    /* PLAY is filtered declaration inventory only; never an independent
     * enabled entry or model scope. Official/local/declared CTL are entries. */
    unsigned paths[DECL_SCOPES];
    int official_mode, local_mode, wake_dispatch;
};
/* Owner STUB: 1 = complete permitted intent/field chain for this scope;
 * 0 = unavailable; negative = invalid. -1 scope asks global availability.
 * Must include enabled connections, script allow_model, declared scope,
 * capabilities, prompt and full extraction chain. Called for -1, official,
 * local or CTL, never the auxiliary PLAY projection. No runtime caps clone,
 * model request, login, HTTP or dispatch is implemented here. */
typedef int (*rec_model_ready)(void *, int scope, const struct rec_channel *);
void rec_defaults(struct rec_settings *);
/* Evaluate ALL affected entries against the SAME cfg/table and proposed
 * settings. Atomic output; no settings write or mutation on any result.
 * The caller must retain inputs/callback dependencies unchanged throughout.
 * Local dispatch is derived: effective off -> direct, effective regex/model
 * -> listen. An archived dispatch value is never an independent choice. */
int rec_validate(const struct decl_table *, const struct rec_settings *,
                 rec_model_ready, void *, struct rec_effective *);
enum rec_status { REC_DISABLED=0, REC_UNRECOGNIZED, REC_DETERMINISTIC,
                  REC_TERMINAL_REJECT, REC_MODEL_PENDING, REC_MODEL_RESULT, REC_MODEL_FLOW_RESULT };
struct rec_result {
    int status, branch;
    struct lx06_match_result match;
};
/* Validate, filter paths, call the EXISTING matcher once, restore original
 * candidate index/B. A miss may report model-pending, never a fabricated
 * model hit. Incomplete/conflict/reject stop; no model fallback on errors.
 * This is recognition only, not mapping, preparation or execution. */
int rec_match(const struct decl_table *, const struct rec_settings *,
              rec_model_ready, void *, enum decl_scope, const char *script,
              const char *text, struct rec_result *);
#endif
