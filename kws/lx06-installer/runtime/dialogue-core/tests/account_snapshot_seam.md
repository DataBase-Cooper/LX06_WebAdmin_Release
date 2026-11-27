# Account identity: offline seam and unresolved compatibility

Status: **not integrated; declaration enforcement is not implemented**. The
existing runner checks a pathname with `lstat`, then asks `/bin/sh` to open it;
the check and open race, and every turn reopens it. The identity regression
reproduces replacement execution in the same session.

A minimally invasive candidate is to capture a bounded script once at session
admission, create a Linux memfd with sealing enabled, write the exact captured
bytes, seal against writes/growth/shrinkage, and parse the same sealed bytes for
its declaration. Keep the descriptor with the epoch; per turn pass it to a
shell and source `/proc/self/fd/N` under `sh -c` with the original path as `$0`
and `--model-request` as `$1`. Seek to the start before each serialized turn;
ensure children cannot corrupt the source and that descriptors are closed on
all failure, retirement, epoch change and destruction paths. Select and retain
mode only from those same captured bytes; reject invalid metadata and reject
any attempt to change script path or selected mode within that epoch. Fail
closed when secure snapshot, seals, full bounded read or declaration parse fails.
Legacy behavior requires a separately designed policy; do not silently degrade
an opted-in account to unbound path execution.

The test `test_account_snapshot_seam.py` establishes only that host `/bin/sh`
can execute sealed synthetic script bytes while preserving basic `$0`, `$1`,
working directory and sibling lookup, even after path overwrite/replacement.
It also demonstrates that reopening `$0` reads the changed pathname, not the
snapshot. A check of pathname/content before launch does not fix that race;
likewise stat metadata, an open source fd, or a digest followed by pathname
execution cannot bind executed bytes. Sourced scripts may differ from executing
a script file in other shell semantics, descriptor use, self-exec and external
resource behavior. Mutable sourced files, subprocesses and resources are not
covered by sealing the top-level script. `memfd_create`, sealing, procfs and
shell behavior must be verified against the actual target kernel, shell and
libc/toolchain, and memory impact against the unresolved OOM before adoption.
This probe proves neither compatibility with real private scripts nor that a
change is detected/rejected; it only proves the interpreter body remains pinned.
A concurrent writer can also alter the file while it is being captured, yielding
a mixed snapshot; matching metadata before/after is not a proof of an atomic
read against an adversarial writer. If strict refusal on any pathname change is
required during capture or a turn, this seam cannot guarantee it without
stronger filesystem control. In particular, path-based self-reads remain unsafe. A production design
must explicitly constrain/validate script behavior or provide a protected,
stable namespace for all path-dependent accesses, with a defined threat model.
