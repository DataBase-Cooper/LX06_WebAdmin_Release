# Dialogue core I/O

This module is provider-neutral. It owns no account, HTTP request, ASR session,
door operation, wake model or official engine. The core owns `dc_tag`, route
priority and state transitions. This code is a development candidate; offline
tests do not accept microphone quality, physical stop latency or final firmware.

## Playback

`player.c` creates one worker for the core lifetime. `alsa.c` loads public
libasound functions and opens playback with `SND_PCM_NONBLOCK`, mono S16_LE,
an 80 ms buffer request and a one-frame start threshold. Supported rates are
8/16/24/48 kHz. The worker owns the handle; other threads never close or drop it.
The PCM queue is fixed at 64 KiB; individual producer writes are at most 4096 B.
Only a maximum 10 ms slice leaves the queue at once. `-EAGAIN` is backpressure,
not permission to drop a sentence or block the microphone loop.

Call `begin(tag, rate)`, enqueue PCM, then `finish(tag)`. FIRST_PCM denotes the
first successful local PCM write, DRAINED denotes the completed device tail.
PCM_RESUMED denotes an actual successful write after at least a 200 ms gap
within the same reply; it is coalesced rather than emitted for every frame.
These events do not automatically change the lights. `stop(tag)` invalidates queued and
worker-local PCM, wakes a dedicated stop eventfd and returns without joining or
waiting for network/synthesis. STOPPED is posted after device drop/close, not a
measurement of residual sound from the physical speaker. Repeated stop is
idempotent. A new generation may start after STOPPED; `begin` returns `-EAGAIN`
while drop acknowledgement is pending. The core must reject stale events too.

Read typed events from `event_fd`/`next_event`; do not run a state/network callback
on the playback worker. Event overflow becomes explicit `ERROR/ENOBUFS`.
Destroy joins only at final core shutdown. Empty unsealed queues do not poll
ALSA POLLOUT, avoiding CPU spin between streamed phrases. All backend operations
must remain local and nonblocking; real plugin/open/drop timing needs device
validation. The ALSA `null` fixture tests the actual host/stock public ABI only.

## Capture

Capture runs in the existing core poll loop with no thread or reader child.
The caller creates the protected directory. LXA1 binds `<name>.sock`; explicit
LXA2 mode binds only `account.a2` and its existing ownership lock. Never open
both for one core. The original PNS tap remains a passive independent producer.
The lock inode survives close; only this instance's socket inode is removed.
The subscription uses nonblocking datagrams and root sender credentials.

LXA1 keeps its original 16-byte header/mono 16 kHz PCM and has no fabricated
producer timestamp. LXA2 preserves its 56-byte header metadata and accepts
160/320/480 samples, carrying producer PID/birth, sequence, availability time and
origin separately from receiver time. Well-framed PCM with invalid metadata is
retained and flagged so the existing endpoint can use its conservative fallback.
Malformed framing/credentials are explicit errors. Sequence discontinuities and
three seconds without a real frame are observable; no synthetic silence/READY.

## Lights

One low-priority worker sends through an inherited, root-peer datagram channel
validated by socket identity, preserving the existing `dialog-light.h` enum.
It duplicates the trusted FD, so closing/reusing the caller's number cannot
redirect output. There is one latest-state slot, with monotonically ordered
epoch and state sequence. Older epochs/sequences and an END for an unopened
epoch are refused. Every active state, including CLOSING and INTERRUPTED, maps
to a valid light. Only the current CLOSED maps to END; destroy/errors do not
manufacture it. Destroy gives the latest explicitly queued state one final
nonblocking send, including a CLOSED accepted immediately before cleanup.

The legacy wire is one byte and does not carry epochs. The receiver must remain
the single serial owner of that session channel; do not insert an asynchronous
RPC queue that can apply an old END after a new epoch. A future shared receiver
needs typed epoch checks at its actual side-effect boundary. The existing owner
now ignores client segment END, shows a new effect before retiring old effects,
and cancels/reaps obsolete worker groups before applying the latest state.
Final owner cleanup alone can extinguish dialogue effects. Other system/night
LED effects and the existing silent flag remain outside this module's control.

## Local verification

Run in WSL as root (the synthetic credential fixtures require root):

```
python3 runtime/dialogue-core/io/tests/test_io.py --output-name UNIQUE_NAME --arm
```

It creates an immutable report directory under `build/rc24-native-tts-offline`.
Host, ASAN/UBSAN and stock glibc 2.25 ARM/QEMU cases use synthetic PCM, UNIX
sockets and a fake ubus ledger in isolated network/mount namespaces. The only
device node used is a private bind of `/dev/null` for ALSA's null plugin. No
speaker, network account, door, DATA or firmware image is modified. ARM lights
use the ARM owner with a static host fake-ubus helper; they do not emulate a
real ledserver. Source and binary hashes are retained separately from real
acoustic acceptance.
