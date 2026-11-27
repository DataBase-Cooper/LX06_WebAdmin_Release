# Opt-in shell-fd-v1 launch contract

`script-settings run-script /absolute/path/demo.sh [argument ...]` reads the script's existing settings declaration and saved values, then starts `/bin/sh /proc/self/fd/9 [argument ...]`. The same retained read-only FD 9 supplies the declaration and the interpreter's source. `/proc` must be mounted. This mode does not execute a script's arbitrary shebang or search PATH for an interpreter.

Existing `script-settings run SOURCE COMMAND ...` is unchanged and remains a general saved-environment overlay. It does **not** bind COMMAND, nested `sh -c`, or subsequent script pathname opens to SOURCE. Existing scripts are not migrated automatically.

An older helper or a refused declaration must not trigger an automatic fallback to ordinary `run`; that would discard the requested binding guarantee. Deployment and the calling script must explicitly select the supported protocol.

The first line must be exactly `#!/bin/sh`. The leading comment block must contain exactly one `# LX06-EXEC: shell-fd-v1` marker and a valid existing `LX06-EXTENSION` declaration. The first executable line must be exactly `exec 9<&-`. Marker and first command must fit within the bounded 16 KiB header scan. For example:

```sh
#!/bin/sh
# LX06-EXEC: shell-fd-v1
# LX06-EXTENSION-BEGIN
# {"version":1,"title":"Example","fields":[{"name":"message","label":"Message","type":"text","env":"DEMO_MESSAGE"}],"actions":[{"id":"save","label":"Save","fields":["message"]}]}
# LX06-EXTENSION-END
exec 9<&-
[ "$LX06_SCRIPT_EXEC_PROTOCOL" = shell-fd-v1 ] || exit 2
[ "${LX06_SCRIPT_PATH##*/}" = "$LX06_SCRIPT_ID" ] || exit 2
# The rest of the script uses quoted arguments and its normal stdin.
printf '%s\n' "$DEMO_MESSAGE"
```

The shell first opens the descriptor path for its own source stream. The required first command then closes inherited FD 9 before any normal subprocess is launched. Native and sanitized actual-helper tests exercise both dash and BusyBox with more than 32 KiB of source after the close command, so this behavior is not inferred from exec success alone. The candidate refuses an already occupied FD 9; it never overwrites that caller descriptor. Other inherited descriptors retain their existing semantics.

The helper overwrites these values after applying saved settings:

| Environment name | Meaning |
| --- | --- |
| `LX06_SCRIPT_EXEC_PROTOCOL` | Exactly `shell-fd-v1` |
| `LX06_SCRIPT_PATH` | The absolute named source path supplied to this invocation and verified against the retained FD before exec |
| `LX06_SCRIPT_ID` | The validated basename used by the existing settings store |

`$0` is `/proc/self/fd/9`, not the original name. It is unavailable for reopening after the required close. A cooperating script uses `LX06_SCRIPT_PATH` for show/save or explicit new launches, and `LX06_SCRIPT_ID` where the original basename is required. It must not use `exec sh "$0"`, source `$0`, or derive the settings identity from `$0` under this protocol.

An explicit self-call uses `exec /usr/lib/lx06/script-settings run-script "$LX06_SCRIPT_PATH" ...`. This is a **new invocation**: it selects the then-current named source and reads the then-current saved configuration. It does not inherit authority to an earlier inode. The helper resets the protocol identity on every invocation. No generated provider script or private uploaded script is rewritten by this candidate.

The argument vector is passed directly, including empty arguments, spaces, quotes and non-ASCII text; no eval or command construction is used. Standard input is not used for source text and reaches the script unchanged. At most 128 script arguments and 65536 aggregate bytes (including the source pathname and NUL terminators) are accepted. Source is a regular, single-link, caller-owned file, not group/world-writable, with size 1 through 1048576 bytes. Symlink final components and nonregular nodes are refused. Existing settings-store missing/corrupt/busy behavior remains distinct; a missing store is not created by this read-only launch.

Identity, permissions, size and modification/change timestamps are checked after declaration parsing and again before exec, while a present settings store's original lock is held. The lock is released before execution, and the source FD alone crosses exec. Atomic source replacement at that boundary therefore runs the selected old inode with its selected configuration. A cooperating publisher must continue using the existing serialized atomic replacement protocol.

This is **inode binding, not a sealed byte snapshot**. A writer able to modify that same inode after the last check can still change bytes consumed by the interpreter. A dedicated actual-consumer test retains this counterexample. The protocol also does not eliminate ancestor-path changes, missing-store writer coordination gaps, or grant execution authorization beyond the original caller's scope. No claim covers arbitrary old scripts or arbitrary `run` command text.

Input/configuration/contract refusal returns 2 without launching the interpreter. A failed interpreter exec returns 127; once exec succeeds, the shell/script's real exit status applies. There is no device, acoustic, login, deployment or global production-registration acceptance in this capability.
