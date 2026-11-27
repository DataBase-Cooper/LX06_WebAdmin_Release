# Status pipe experiment — incomplete; do not integrate or deploy

This isolated fork-only transport experiment is **not production usable** and
is not connected to `session-run`, `lx06-dialogued`, an actual capture overflow,
or webd. It exists to make a narrower contract testable, not to claim the
requested supervisor handoff is complete.

One serialized owner generates 128 random bits from `/dev/urandom` per begin,
opens a nonblocking close-on-exec anonymous pipe, and accepts only the fixed
20-byte record containing that token and the single overflow enum. Last error is
retained after end and cleared by the next begin. A previous token sent through
the new pipe is ignored. An inherited writer is a capability: this does not
authenticate a sender if descriptors or tokens leak. There is no locking.

Host Linux/WSL (UBSan) and static ARM under QEMU exercised an actual forked
child, a child killed after its write, a mismatched prior token, and actual
pipe saturation/EAGAIN followed by draining and retry. The overflow is an enum
injected by a fixture, **not** the core's actual ENOBUFS path. The supervisor
is the test process, **not** the actual `session-run`. No exec, stock ARM ABI,
device, network, acoustic path or web API was tested.

Before any integration: solve secure exec/bootstrap and direct-parent ownership
(the current supervisor launches a generic script tree); implement actual
lifecycle coordination and retention across distinct session-run processes;
prove reliable nonblocking delivery for a terminal overflow including full
pipe, reader death/SIGPIPE and simultaneous crash; validate truncation,
malformed streams, descriptor inheritance and stale writers; and exercise real
capture overflow through the real supervisor subprocess seam. A full pipe may
currently lose the only terminal report, and SIGPIPE may terminate the writer.
Existing CLOEXEC intentionally prevents an execed core from inheriting this
experimental writer. No persistence or observer channel exists. Do not interpret
absence of a report as absence of overflow.
