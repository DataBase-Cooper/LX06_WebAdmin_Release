# Capture status attribution: offline boundary and design

The real `session-run` waits for its direct launched child, which may be a shell
or another arbitrary executable. It returns that child's 75 unchanged; it also
returns 75 on admission/lock refusal. Therefore a supervisor return of 75 is not
a capture-overflow notification. The legacy script path must remain unchanged.

`test_supervised_capture_attribution.py` builds the production supervisor and
production core `main.c` on Linux. Its capture/ASR/route are synthetic: the
route boundary returns ENOBUFS, rather than filling a real retained-audio ring.
The separate `test_capture_overflow.c` exercises the actual capture function and
bounded route. This fixture pauses the direct core child, checks its live
`/proc/<pid>/exe`, parent and lease birth tick, then resumes and observes the
supervisor returning 75 and the core's overflow diagnostic. A separate generic
script produces the same supervisor return. A second generation has different
live lease identity. This is observer-side test evidence, **not** authentication
at production exit: exec can change an image without changing PID or birth tick,
a pathname may change, and the proc snapshot is not an atomic exit attestation.
No stale status was sent or rejected in this harness.

A possible future separate status transport must first establish a trust boundary:

* Keep ordinary script launches and their exit codes exactly as they are; an
  arbitrary script, its children, and lock-contention 75 remain UNKNOWN.
* Use a distinct supervisor-controlled direct-core launch mode, pin an
  administrator-controlled executable by an opened, verified file identity and
  `fexecve` (or a verified equivalent on the target), and define how writable
  executables, replacement, loader, interpreter, environment, ptrace and
  same-UID processes are excluded. The private script path cannot confer trust.
* Have the supervisor create a fresh per-session private IPC channel itself;
  do not give the sending endpoint or secrets to scripts or other descendants.
  A nonce or PID alone does not authenticate an executable. Per-message kernel
  credentials plus direct-child PID and birth identity can support attribution,
  but do not by themselves prove the sender's executable image; a core contract
  must exclude unsafe exec/descriptor passing or use a stronger OS boundary.
* Bind records to the currently held lease and an independently generated unique
  session generation, validate fixed lengths/types and sender identity, accept
  only while that generation is active, and invalidate/drain on termination.
  Treat missing, malformed, overflowed, late or unverifiable messages as unknown,
  not as evidence of capture overflow. Never let an old message mutate a new
  session. Check descriptor lifetime across fork, exec, and all descendants.
* Before production use, build negative integration tests for generic and
  malicious scripts, inherited/forwarded descriptors, spoofed kernel credentials,
  executable replacement/exec races, old-generation replay, PID reuse, early
  child death, channel backpressure, lease handoff and cleanup. Establish the
  actual target kernel/glibc features and threat assumptions, including root and
  same-UID adversaries. No such secure transport is implemented here.

The fixture neither tests a physical microphone nor proves a user-visible notice,
production IPC authentication, stale-message rejection, deployed ARM behavior,
or an acoustic result. Test deadlines are bounds, not latency measurements.
