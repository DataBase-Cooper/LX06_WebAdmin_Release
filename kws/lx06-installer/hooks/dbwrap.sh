#!/bin/sh
# Keep the Dropbear PID for procd. A private FIFO feeds the 64KiB sink without
# making SSH depend on Web88 polling or a persistent DATA log.
if ! grep -q '^log_ssh=0' /data/lx06-ssh.conf 2>/dev/null; then
  logdir=$(mktemp -d /tmp/lx06-ssh-log.XXXXXX)
  if [ -n "$logdir" ]; then
    fifo=$logdir/stderr
    if mkfifo -m 600 "$fifo"; then
      (
        exec 3<"$fifo" || exit 1
        rm -f "$fifo"
        rmdir "$logdir"
        exec /usr/bin/lx06-keeper log ssh <&3 3<&-
      ) &
      trap '' PIPE
      exec /usr/sbin/dropbear "$@" 2>"$fifo"
    fi
    rmdir "$logdir"
  fi
fi
exec /usr/sbin/dropbear "$@" 2>/dev/null
