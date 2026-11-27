# Script model-understanding contract

This is an unreleased speaker-side source interface. No account, device, audio,
deployment, or firmware acceptance is implied by host synthetic tests.

The original extension runner selects an uploaded **script filename**, reads its
`structured` declaration, obtains a fresh status observation, and invokes the
same script's declared action. Firmware does not recognize provider names. The
filename may be changed without modifying firmware. A script without a valid
declaration is unavailable; ordinary chat success does not imply this capability.

`account_structured(schema, ...)` adds five transient carriers plus `extract`
and `understand`. Call it after the script's UI flow and save field lists have
been created. The carriers must have no defaults/env mappings and must not be
saved. The existing twelve-action/twenty-four-field limits remain enforced.
This two-action capability supports a combined intent+extraction connection:
`understand` returns scores and selected fields, while `extract` supports the
selected-intent fallback. It does not declare a separate `intent` action.

Default mapping is `status` / `invocation`, `request`, `prompt`, `response`,
`status`, with successful status value `logged_in`. The status action name,
carrier field names, and status value may be changed through the Python helper
arguments. Set the corresponding `STRUCTURED_*_FIELD` shell constants (and
`STRUCTURED_LOGGED_IN_VALUE`) to the same names. Model action names `extract`
and `understand` are the original protocol enums, not arbitrary aliases.

Embed `account-structured.sh` at packaging time into the same upload. It is a
build-time module, not another resident service or a runtime source dependency.
The upload supplies its original `prepare`, `field`, `get`, `put`, `fail`, `TOOL`,
and cleanup functions. `TOOL` is the existing bounded native model-tool. `prepare`
reads the original `{name,action,values}` stdin into its private request directory
and makes `WORK` available. There are two text-only callback functions:

* `structured_account_ready`: return successfully only after the script's
  existing account/credential check succeeds. Do not automatically log in.
* `structured_text_infer`: read `WORK/structured-system` and
  `WORK/structured-request`, call the script's **existing text inference path**,
  and write exactly one complete `{"text":"..."}` result to
  `WORK/structured-answer`. Return failure on transport/parser failure, refusal,
  incomplete response, or over-limit data. Never invoke audio or business code.

The actual encoded provider request must be at most 8192 bytes. Check this after
JSON escaping and provider metadata, before submitting inference. The shared
module also checks the request (8192), optional prompt (2048), complete response
(8192), and final extension event (8192). There is no truncation or JSON repair.
It verifies the original request ID, and the existing model chain remains the
authority for field types, allowed intents, scores, roster mapping, and current
ownership. No provider-supplied rows or authorization become local business data.

Dispatch `extract|understand` to `structured_request "$action"` and the mapped
status action to `structured_status`. An empty status invocation keeps the
upload's existing UI `account_status` behavior; a nonempty invocation requires a
fresh usable account result and is echoed unchanged. Emit only the final event.

ChatGPT/Grok use `account-common.sh`'s `account_text_infer`, shared with their
existing text-test action. Provider headers, request_options, login/refresh and
SSE validation stay in that existing path. DeepSeek's callback reuses `ds_infer`
and native `--collect`; only its explicit structured entry uses the prepared
prompt. Ordinary conversation `--prompt` limits and voice parsing are unchanged.

The generated existing account uploads re-enter their own script through the
original SETTINGS `run` helper with `--configured` for these two explicit
actions. This reads the already-saved model without consuming stdin or acquiring
another EX lock. The generic runner is unchanged. Other scripts can retain their
own settings mechanism. Never infer that an externally deployed older upload
has these declarations until its actual source is inspected/upgraded.

`structured-provider-template.py` generates a standalone, deliberately disabled
example with alternate carrier names and a custom status action. Replace its two
callbacks with an existing provider script's text/status functions. This applies
equally to future Gemini, Claude, or other uploads; no new provider login or live
account support is supplied by this template.
