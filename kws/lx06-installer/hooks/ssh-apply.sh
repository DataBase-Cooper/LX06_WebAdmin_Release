#!/bin/sh
# ssh-apply.sh — COMPAT WRAPPER. All SSH wiring moved to the single read-only
# /usr/lib/lx06/ssh.sh (SSH config: port / root password / LAN-only). This
# wrapper exists so the read-only /etc/init.d/boot() tail (which still calls
# ssh-apply.sh before S50 dropbear) and any external references keep working:
# it delegates straight to ssh.sh. SSH is NOT defined here — see ssh.sh.
exec sh /usr/lib/lx06/ssh.sh "$@"
