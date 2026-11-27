"""Offline sealed-session regression against the production account runner."""
import ctypes as C
import fcntl
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_account import AccountTest


class SealedSessionTest(AccountTest):
    def test_rejected_unsealed_binding_cannot_fall_back_to_legacy(self):
        fd = os.memfd_create('unsealed', os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, fd)
        os.write(fd, b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: voice\n')
        bind = self.lib.dc_account_bind_sealed_v1
        bind.argtypes = [C.c_void_p, C.c_uint64, C.c_int]
        self.assertEqual(bind(self.a, 1, fd), -1)
        self.assertEqual(self.start(), -1)

    def test_sealed_session_survives_replacement_and_reopens_dollar_zero(self):
        self.scriptpath.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: keyword
[ "$1" = --model-request ] || exit 91
case "$0" in /proc/self/fd/*) ;; *) exit 92;; esac
[ "$(grep -xc '# LX06-INTERRUPTION: keyword' "$0")" = 1 ] || exit 93
printf '%s\\n' '{"delta":"original"}' '{"done":true}'
''')
        fd = os.memfd_create('test-account', os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, fd)
        os.write(fd, self.scriptpath.read_bytes())
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        bind = self.lib.dc_account_bind_sealed_v1
        bind.argtypes = [C.c_void_p, C.c_uint64, C.c_int]
        self.assertEqual(bind(self.a, 1, fd), 1)
        self.assertEqual(self.start(), 0)
        events = self.drain()
        self.assertEqual([e['text'] for e in events if e['type'] == 1], ['original'], events)
        replacement = self.path / 'replacement.sh'
        replacement.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"delta":"replacement"}\' \'{"done":true}\'\n')
        replacement.replace(self.scriptpath)
        self.assertEqual(self.start(generation=2, utterance=2), 0)
        events = self.drain()
        self.assertEqual([e['text'] for e in events if e['type'] == 1], ['original'], events)
        self.scriptpath.write_text('overwritten in place\n')
        self.assertEqual(self.start(generation=3, utterance=3), 0)
        events = self.drain()
        self.assertEqual([e['text'] for e in events if e['type'] == 1], ['original'], events)
