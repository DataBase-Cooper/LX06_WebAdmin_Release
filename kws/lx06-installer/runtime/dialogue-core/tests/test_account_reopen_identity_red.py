"""Offline RED: a declared script must not reopen replaced $0 during a session.

Synthetic files only. A future supported execution contract may instead reject
this script before running it; it must not execute the replacement's content.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_account import AccountTest


class ReopenIdentityRed(AccountTest):
    def test_reopen_after_running_script_is_replaced(self):
        ready = self.path / 'ready'
        proceed = self.path / 'proceed'
        self.scriptpath.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: keyword
touch "$(dirname "$0")/ready"
while [ ! -e "$(dirname "$0")/proceed" ]; do sleep 0.01; done
if grep -q ORIGINAL_BODY "$0"; then answer=original; else answer=changed; fi
printf '{"delta":"%s"}\\n{"done":true}\\n' "$answer"
# ORIGINAL_BODY
''')
        if self.start() != 0:
            return  # Refusal is acceptable for this unsupported contract.
        deadline = time.monotonic() + 2
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(.005)
        self.assertTrue(ready.exists(), 'script did not reach reopen barrier')
        replacement = self.path / 'replacement.sh'
        replacement.write_text('#!/bin/sh\n# replacement\n')
        replacement.replace(self.scriptpath)
        proceed.touch()
        events = self.drain()
        self.assertNotIn('changed', [e['text'] for e in events if e['type'] == 1])

    def test_declared_script_reopening_zero_cannot_observe_replacement(self):
        self.scriptpath.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: keyword
if grep -q ORIGINAL_BODY "$0"; then
  printf '%s\\n' '{"delta":"original"}' '{"done":true}'
else
  printf '%s\\n' '{"delta":"changed"}' '{"done":true}'
fi
# ORIGINAL_BODY
''')
        first = self.start()
        if first != 0:
            return  # A contract that blocks scripts which reopen $0 is safe.
        self.assertEqual([e['text'] for e in self.drain() if e['type'] == 1], ['original'])
        replacement = self.path / 'new-script.sh'
        replacement.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: voice
printf '%s\\n' '{"delta":"replacement"}' '{"done":true}'
''')
        replacement.replace(self.scriptpath)
        if self.start(generation=2, utterance=2) == 0:
            self.assertFalse({'changed', 'replacement'} &
                             {e['text'] for e in self.drain() if e['type'] == 1})
