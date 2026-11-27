# Isolated session-status experiment (not wired)

`session-status.c` is a fixed-size in-memory state machine, not a core-to-web
transport. A trusted, serialized owner begins a session using supervisor and
process-group PIDs and their birth ticks. Only that active identity can report
the fixed `capture_overflow` enum. Ending retains the last error for reading;
the next distinct begin clears it. Reports for the prior identity are rejected.
No text or credentials are stored.

The existing supervisor inherits a lease into a generic shell process tree,
but has no established core error channel or retained supervisor-to-web status
channel. This experiment does not authenticate callers, prove a process or FD
belongs to a lease, defend against forged begin/end calls, serialize access,
persist data, survive a supervisor restart, or publish to the web API. Identity
values supplied by an untrusted caller are not proof of identity. Never wire
this seam without a trusted per-session channel, atomic lifecycle/publication,
and real-process tests including late senders and next-session races. In
particular a trusted coordinator must prevent an old begin being replayed after
an intervening session; the bounded state machine only remembers the most
recent identity. The core now exits 75 (EX_TEMPFAIL) when capture routing returns
ENOBUFS; other runtime failures retain exit 1. This is a process exit contract,
not a retained diagnostic: only interpret 75 when the observed child is known
and authenticated to be the core for that exact session. Generic scripts and
shell wrappers may also exit 75, propagate it, mask it, or reuse old stderr, so
neither their status nor an old log line proves overflow. A supervisor must not
classify a generic script's status as a core error. The current supervisor and
web UI are not wired to consume this contract; there is no user notification.
A later integration requires a trusted per-session channel or equivalent direct
core identity and lifecycle binding; it must clear previous status at the start
of each session and reject stale reports.

The standalone C test exercises the actual seam, including stale reports,
unknown errors, duplicate begin, retained error and clearing on a new session;
it is not an IPC, supervisor, web API, or hardware acceptance test.
