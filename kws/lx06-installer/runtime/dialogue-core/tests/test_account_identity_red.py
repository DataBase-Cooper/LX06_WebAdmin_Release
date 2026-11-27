"""RED: an opt-in account's declaration must stay bound to its executable.

Offline synthetic files only. This intentionally fails until session-scoped
identity binding is implemented; it is not a production implementation.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_account import AccountTest


class AccountIdentityRed(AccountTest):
    def test_opt_in_script_replacement_cannot_change_active_session(self):
        # The original observes its real $0 and a sibling resource. Any future
        # snapshot scheme must preserve those observable semantics for opt-ins.
        (self.path / 'resource').write_text('original')
        self.scriptpath.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: keyword
[ "$0" = "$EXPECTED_SCRIPT" ] || exit 91
[ "$(cat "$(dirname "$0")/resource")" = original ] || exit 92
printf '%s\\n' '{"delta":"original"}' '{"done":true}'
''')
        import os
        previous = os.environ.get('EXPECTED_SCRIPT')
        os.environ['EXPECTED_SCRIPT'] = str(self.scriptpath)
        self.addCleanup(self.restore_env, previous)
        self.assertEqual(self.start(), 0)
        events = self.drain()
        self.assertEqual([e['text'] for e in events if e['type'] == 1], ['original'])
        self.assertEqual([e['type'] for e in self.terminal(events)], [3])

        replacement = self.path / 'replacement.sh'
        replacement.write_text('''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: voice
printf '%s\\n' '{"delta":"replacement"}' '{"done":true}'
''')
        replacement.replace(self.scriptpath)
        rc = self.start(generation=2, utterance=2)
        if rc == 0:
            events = self.drain()
            self.assertNotIn('replacement', [e['text'] for e in events if e['type'] == 1],
                             'replacement adapter executed under the original session')

    @staticmethod
    def restore_env(previous):
        import os
        if previous is None:
            os.environ.pop('EXPECTED_SCRIPT', None)
        else:
            os.environ['EXPECTED_SCRIPT'] = previous
