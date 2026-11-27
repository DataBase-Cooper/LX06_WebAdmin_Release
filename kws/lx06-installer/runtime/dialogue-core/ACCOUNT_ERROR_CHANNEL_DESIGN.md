# Account-core structured error propagation: design only

**Decision: do not change production.** The current supervisor accepts generic
script trees, which can run arbitrary code with the launcher's authority. No
existing evidence binds the actual `--account` core executable, its protected
sender, and the lease against that authority. Until this is established the UI
must retain **unknown**, not attribute exit 75, stderr, absence of a message, a
PID snapshot, or a script-supplied record to capture overflow. This is an
architecture proposal and an isolated mock, not a working production feature.

## Relevant current path

`tests/generated_account_declaration_experiment.md` traces the generated launch:
`voice_session` -> `session-run` -> `sh "$0" --session-worker` ->
`voice_worker` -> `lx06-dialogued --account "$0" ...`. The wrapper and its
script descendants are not authenticated core executables. A generic child and
lease admission can both return 75, and wrappers can swallow it. See
`tests/supervised_capture_attribution_design.md` for the separate injected
`--audio` supervisor fixture and its limits.

`main.c:capture` calls `dc_audio_route_capture` only inside `if(c->audio)`;
`--account` is not `--audio` (`main.c` 496-508, 224-227). Thus this particular
ENOBUFS branch does not establish an account-mode overflow at all. Account-mode
failures need their own real cause traced and a core-owned enumerated error site;
do not label them capture overflow by analogy. The 128 KiB retained audio ring
and any result of its audio-mode fixture cannot by themselves justify an
account-specific UI message. No promise of user notification exists today.

## Required boundary before implementation

1. A privileged, persistent serialized coordinator owns admission, the lease,
   generation, direct-child lifecycle, and publication. For the special account
   core path it independently selects and launches an administrator-controlled,
   immutable verified core image, loader and dependencies, using a target-verified
   fd-based exec or equivalent. Its identity is not a pathname, live proc snapshot,
   script claim, magic value or exit status. Protect credentials, executable bytes,
   environment, loader, `/proc`, ptrace, and descriptor access from script code;
   use separate UIDs/OS confinement or another demonstrated boundary. Root and
   privileged tampering are outside this proposed boundary unless explicitly
   mitigated. Sealed script contents do not establish this boundary or prevent
   later execution, sibling-resource access, or shared-flock manipulation.
2. Only the coordinator creates a fresh AF_UNIX socketpair per admitted launch;
   retain receiver, deliver the sender directly to the verified core across exec
   using an explicit FD allowlist, close it everywhere else, and set close-on-exec
   again immediately in the core. All script/model/worker forks and execs must
   close it before untrusted code can access it. An inherited FD is authority;
   a random nonce, SO_PEERCRED at socketpair creation, or an environment secret
   cannot stop code that possesses or steals it. Require kernel per-message
   SCM_CREDENTIALS (SO_PASSCRED) checked against the still-owned direct child;
   establish birth/ownership without PID-reuse races and verify target semantics.
   Credentials identify a process, not its image or whether it execed arbitrary
   code. The core must not exec untrusted code in that process, permit ptrace,
   leak the FD using SCM_RIGHTS, or let same-UID processes obtain it. If those
   invariants cannot be enforced, refuse attributed reports and stay unknown.
3. At admission create a cryptographically random 128-bit launch generation,
   independent of the core epoch, and bind it atomically to the lease and child.
   Use a bounded fixed-format versioned record: magic, version, generation,
   sequence and allowlisted enum; choose explicit endianness/size and reject
   malformed, truncated, extra, unsupported, duplicated, out-of-order or stale
   records. Never accept arbitrary display text, paths, PCM, credentials or
   provider messages. The enum may be emitted only by the *actual* audited
   account error branch, not by a generic exit. Version, limits, state machine and
   failure semantics must be reviewed before selecting production wire numbers.
4. On any uncertain ownership, invalid packet, failed send, channel overflow,
   peer death, crash, full queue or unknown mode, fail closed to unknown; never
   infer successful operation from missing data. Sending must not block audio or
   terminate on SIGPIPE. A bounded terminal-report retry/ack protocol and its
   deadline, queue-full behavior and failure reporting need real tests; no IPC
   scheme can guarantee notification if both reporting and persistence fail.
5. The coordinator serializes admission, report, termination and publication.
   While the exact generation and lease remain current, latch one fixed event ID
   and enum. Closing a session retains that event until ACK or a new admission;
   admission clears prior state under the same lock. A stale end, old sender,
   replay, delayed publication or old ACK cannot modify the new generation.
   Use an authenticated, access-controlled web endpoint: non-consuming GET
   returns generation/revision, event ID, and enum or unknown; explicit ACK
   compares the exact ID atomically. UI maps the enum to local fixed text and
   fences asynchronous GET/ACK responses by current generation *and* request
   revision, including across concurrent tabs. Polling is at-least-once, not
   exactly-once. Missing or uncertain state is unknown, not success.
6. A coordinator restart invalidates all prior channels, children and generations;
   do not restore a positive status without an authenticated durable journal and
   atomic recovery protocol. Choose lock/storage ownership, directory protections,
   crash consistency, web restart recovery and fencing of orphan processes before
   implementation. If a script can run with coordinator authority or interfere
   with its transport, this architecture is not deployable under that threat.

## Mock evidence and remaining tests

Run on Linux: `python3 runtime/dialogue-core/tests/test_status_channel_mock.py`.
The standalone test exercises an actual anonymous datagram socketpair and Linux
per-message credentials with a **mock** coordinator and UI state. It checks a
simulated trusted sender, generic-script refusal even with matching PID,
forwarded FD from a different forked PID, stale generation/end/ACK, duplicate,
malformed and late messages. Its Boolean `trusted_direct_exec` is a test
assumption: it performs no exec authentication, confinement, lease, production
core error, supervisor, real HTTP/UI or reliability integration. Matching PID
would not protect against arbitrary code executed in that same process. It does
not test PID reuse, race-free process identity, credential forgery/privilege,
SCM_RIGHTS, ptrace, concurrent calls, queue saturation, crashes, UI polling,
actual account behavior, target kernel/libc, acoustic behavior or hardware.

Before implementation, add real-core-to-real-supervisor-to-real authenticated UI
fixtures with hostile generic scripts, exact lifecycle and generation races,
replacement/exec, parent death, leaked/inherited/passed FDs, privilege/same-UID
attack attempts, PID reuse, malformed and saturated sockets, EAGAIN/SIGPIPE,
crash/restart at every commit boundary, old GET/ACK responses, and fault-injected
actual account failure. Verify deployed ARM kernel and ABI separately. Until
then **unknown** is the only honest attribution for the existing generic path.
