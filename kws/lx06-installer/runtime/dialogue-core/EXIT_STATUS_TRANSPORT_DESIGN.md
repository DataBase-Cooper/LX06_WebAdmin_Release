# Fixed-code session failure exposure: design only

No user-facing transport is implemented or accepted. The core currently returns
1 for all failures after initialization, including capture overflow, so this
status does **not** distinguish overflow. The standalone overflow fixture checks
production capture and the fixed diagnostic, not the full reactor's OS exit.
The real-process regression checks a different, generic startup failure.

## Candidate contract

- For an explicitly authenticated and supervised core launch only, reserve a
  distinct exit code for capture overflow. Select it after auditing collisions
  with every launcher, script and supervisor exit code. Preserve generic failure
  as generic; a signal, timeout, memory pressure, or uncertain/ambiguous child
  provenance must never be reported as overflow. Never surface stderr, script
  names, arguments, or arbitrary strings; UI renders a local fixed message.
- `session-run` currently waits for its root child, which may be a generic script
  tree rather than the core; it returns that child's status unless overridden
  by preemption, timeout or memory pressure. A script can swallow or forge an
  exit code. Neither the code nor a process-group membership authenticates the
  core. Require a production-defined trusted direct-child/exec binding to the
  actual core, compatible with each supported launcher, before interpreting a
  reserved code. The core's own epoch is not currently a supervisor event ID.
- Serialize session admission and event publication across distinct supervisor
  processes under a security-checked shared owner lock. Assign a fresh, unique
  supervisor generation at admission; atomically publish only while that same
  generation remains current. A delayed old exit must not update the replacement
  session, including ABA or supervisor crash/restart. Keep lifetime and storage
  bounded and protect against path replacement, unauthorized writers, and
  partial writes; define recovery when the owner or web server dies. Clearing
  the lease file currently removes its metadata, so it is not a retained channel.
- Expose only a fixed enum and opaque generation/event ID through the existing
  authenticated web service. A GET is non-consuming and idempotent; explicit
  authenticated ACK compares the exact event ID under the same serialization
  and removes only that event. Concurrent polls may display it more than once;
  delivery is at-least-once until ACK, not exactly-once. A stale ACK must not
  consume a newer event. Define the UI policy for replacement: invalidate a
  previous generation when a new session is admitted; late fetches and ACKs
  cannot resurrect or dismiss the new state. If publication is lost or recovery
  uncertain, report unknown, not success. Do not silently treat absence as proof.

## Acceptance still required

Exercise the real `lx06-dialogued` overflow in a real process through the real
`session-run` launch, lease, all supported launcher chains, authenticated Web API
and actual UI handler; include normal failure, signals, preemption and competing
exits. Gate the old supervisor until a new session has replaced it, then release
it and prove the old exit/response/ACK cannot mutate or erase the new status.
Test concurrent observers, retries, crashes at each commit boundary, no-reader
and loss cases, identifier reuse, malicious script status and malformed traffic.
Stock-ABI and on-device behavior require separate acceptance. No live device,
network, OTA, or acoustic acceptance is implied by the current offline tests.
