# Unified reactor verification

`test_control.c` and `test_session.c` exercise provider-independent decisions.
`test_phone_recognition.py [--sanitize]` feeds the production recognition
function synthetic ASR partials/finals while a phone cue is active. It checks
old generations, finalized serials rebound to the current generation, invalid
UTF-8, and valid interruption without submitting a partial as a question. It
checks cue state, not a physical speaker; the mode reactor covers the synthetic
player drop on valid interruption. The recognizer interface supplies generation
and serial; recognition itself attaches the current core session epoch.
`test_interrupt_integration.py [--sanitize]` runs real session voice decisions,
reactor stop/cancel/question callbacks, audio route and player worker with a
synthetic PCM sink. A gated local drop and independently withheld route ACK
exercise cancellation ordering, retained capture bytes and next-player ownership;
the ACK and capture are injected, not a real adapter, ASR, microphone or speaker.
A final mixed goodbye during a playing greeting also drives the production stop
and cancellation callbacks and worker: it requires exactly one stop, cancel and
close, with backend drop independently gated. The close callback is synthetic;
this verifies request ordering, not physical silence or device latency.
`test_cancel_ipc.py [--sanitize]` fills a nonblocking Unix socket to EAGAIN,
then exercises the real session/reactor, player worker, audio route and production
adapter-event dispatcher. It injects cancel ACK, mismatched and matching resume,
and old reply begin/audio/end over the socket; verifies the local synthetic drop,
barrier, bounded capture/ENOBUFS and byte-exact finalized next utterance. The
peer and playback backend are synthetic, not a provider, microphone or speaker;
this does not measure physical silence or end-to-end interruption latency.
The same runner includes `test_capture_overflow.c`: it executes production
`capture()`, route, session, `send_microphone()` and adapter dispatch, with
synthetic capture, ASR and voice-guard boundaries. A 96,000-byte (three seconds
at 16 kHz mono s16le) example is retained and delivered byte-for-byte on a
matching resume. This is a test input, not a configured maximum utterance.
The overflow test fills the ring through production `capture()`, then observes
that the next 320-byte frame emits the fixed `capture_overflow`/ENOBUFS diagnostic
and sets that core's `failed` flag. A matching late resume cannot release or send
retained PCM after failure. Closing one of two in-memory session objects does not
alter the other. This is not a supervisor, lease, process-lifetime or simultaneous
real-session handoff test.

A stalled cancellation cannot preserve unlimited speech with bounded storage:
128,000 bytes is at most four seconds of this PCM, including any retained
pre-roll and all subsequent frames, including silence. With an active cancel
barrier, capture refuses the entire frame that does not fit with `-ENOBUFS`;
`capture()` feeds that frame to the local ASR before the route rejects it, sets
`failed`, and the reactor closes the session on its next loop. Retained bytes
are not a successful complete question after that failure. A separate route
watchdog reports `-ETIMEDOUT` only when cancellation age is strictly greater
than 15,000 ms; whichever failure is reached first wins in the running loop.
This is capacity arithmetic and a synthetic timeout boundary, not measured
latency. The diagnostic is stderr metadata, not a verified spoken/UI notification;
the reactor has no established always-available local notification path for audio
sessions (its TTS bootstrap is used only by non-audio modes). A user-facing retry
notice therefore remains unimplemented. After failure the reactor skips further
recognition and audio dispatch and shuts down; it cannot retract PCM that had
already crossed the adapter boundary before the overflow.
The real-process `process-exit-status` reactor case separately checks missing
startup endpoints return 1 after reaching capture initialization, `--protocol`
returns 0, and invalid arguments return 2. Other reactor cases exercise a clean
session exit. This is a generic startup failure, NOT an actual overflow exit:
the overflow test above calls production capture code in a fixture process and
does not run the full main loop. Exit 1 currently conflates multiple failures.
See `../EXIT_STATUS_TRANSPORT_DESIGN.md` for the missing supervisor/UI transport.
`test_reactor.py` executes the actual `main.c` reactor, shared ASR transport and
Opus encoder, IO player/capture/lights, account parser, TTS decoder controller,
voice guard and vendored VAD. It replaces only external endpoints with local
synthetic services. `fake-asound.c` implements the published playback ABI with
an 80 ms queue drained according to monotonic time; its ledger records actual
accepted PCM and drop operations. It never opens audio hardware.

Run from the repository in WSL as root:

```
unshare --net python3 runtime/dialogue-core/tests/test_reactor.py --output-name UNIQUE_REACTOR
unshare --net python3 runtime/dialogue-core/tests/test_reactor.py --output-name UNIQUE_ASAN --sanitize
python3 runtime/dialogue-core/tests/run_stock.py --output-name UNIQUE_STOCK --arm-output build/UNIQUE_ARM
```

Every output directory is new and immutable. Reactor reports retain the core
build manifest, binary hash, fixture source hashes, numeric events, light
states and queue/drop measurements. The test-only ownership permit exists in
host fixture builds alone. `run_stock.py` checks a production ARM ELF against
stock glibc 2.25, then runs protocol discovery and missing-lease refusals under
QEMU in temporary stock-image chroots with private mount/network namespaces.
An existing immutable binary can be supplied with `--binary` to either runner.
Changing any relevant source after a build makes its final report fail instead
of treating an earlier binary as validation of the current tree.

The nine reactor cases include the exit-status check and check complete questions instead of partials; the
first actual framed PCM through Opus; two streamed segments and their light
gap; single/repeated control words with a subsequent complete question; mixed
end words closing once; generation-zero TTS warm teardown; and stale PCM/finals
that cannot affect the next reply. Control-to-drop timing starts when a fake
recognition observation is sent. It excludes real ASR recognition, acoustic
echo, cloud transport, hardware buffers and speaker decay. These tests provide
no acoustic acceptance or proof of real-device latency/long-term stability.
No real account, speaker, microphone, DATA, firmware or door is accessed.

The generation-zero warm-close case alone preloads a syscall-based monotonic
clock fixture, advancing 1 ms on each read. This deterministically makes the
loop's earlier time snapshot precede the teardown timestamp. It guards the
unsigned-subtraction bug that used to trigger an immediate cleanup timeout.
Other reactor cases use the ordinary monotonic clock and timed ALSA queue.

`test_media.py` checks the no-thread/no-file cue generator on host, sanitizers
and stock ARM. It verifies cue identity/role, 10/20 ms framing, bounded prefill,
stop/restart, cadence, dual-frequency spectrum and gentle peak/envelope.

```
python3 runtime/dialogue-core/tests/test_media.py --output-name UNIQUE_MEDIA --arm
```
