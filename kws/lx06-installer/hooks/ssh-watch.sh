#!/bin/sh
# ssh-watch.sh — COMPAT WRAPPER. The LAN-only iptables filter + dropbear
# keepalive moved into the single read-only /usr/lib/lx06/ssh.sh (Part B,
# backgrounded). This wrapper keeps the staged /data/ssh-watch.sh (started by
# init-launch.sh) and any external references working: it delegates to ssh.sh.
# The LAN-only semantics live in ssh.sh now, not here.
exec sh /usr/lib/lx06/ssh.sh "$@"
