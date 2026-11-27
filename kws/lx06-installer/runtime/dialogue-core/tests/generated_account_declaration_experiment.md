# Generated account v1 declaration experiment — unconnected

`runtime/model-adapter/build_accounts.py DEST --experimental-account-declaration keyword`
(or `voice`) adds only the two parser-defined header comments to the generated
ChatGPT and Grok uploads. Without the option generation remains legacy; the
body is identical. This is **not** a sealed launcher, a security attestation,
or an enabled controller path. The declaration alone does not change legacy
`--account` policy. Do not deploy these experimental outputs as proof of
session identity or interruption behavior.

## Trace and blocker

- `--wakeup` enters `voice_resident`/`voice_session`; the latter uses `$0` as
  the settings identity and invokes `session-run` with `sh "$0"` and the
  basename. The `--session-worker` wrapper again uses `$0` both as settings
  key and as the script to re-execute, then `voice_worker` launches the core
  with `--account "$0"`.
- The controller's opt-in binder requires an inherited, fully sealed memfd,
  parses its bytes, and executes `/bin/sh /proc/self/fd/N --model-request`.
  It exists only when a trusted upstream owner supplies that descriptor; the
  generated launcher currently supplies no descriptor. Replacing `$0` by the
  fd path everywhere changes settings identity, resident lock/event names and
  re-execution semantics; preserving the original pathname instead allows its
  contents to change. No safe end-to-end binding has been established.
- The `--model-request` branch invokes `prepare`, reads and may refresh mutable
  provider credentials under `/data/lx06/model-accounts`, uses a lock and
  private temporary files/FIFO, executes the external model tool and curl or
  inherited HTTP helper/socket, and depends on provider endpoints and request
  input. `--speed-runtime` may append helper payload logic. These dependencies
  are not sealed by the declaration or the script memfd. Separately, web actions
  use `$0` for settings/options, uninstall, and the greeting path; they are not
  a demonstrated immutable execution boundary either.
- No production launcher change was made. The previous independent security
  review remains blocked; this experiment does not clear it. A usable design
  still needs a trusted owner, same-fd lifetime/re-entry proof, a stable
  separately authenticated settings identity and an explicit trust boundary for
  mutable resources, followed by real generated-script and real-main offline
  acceptance. No device, network, or OTA operations are covered here.
