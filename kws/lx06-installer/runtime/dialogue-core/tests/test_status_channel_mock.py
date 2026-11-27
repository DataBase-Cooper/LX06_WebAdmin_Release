#!/usr/bin/env python3
"""Design-only Linux socketpair mock; never imported by production code.

The trusted-exec boundary is an ASSUMPTION in this fixture, not verified by it.
No real core, supervisor, lease, web handler, filesystem socket or device is used.
"""
import os
import socket
import struct
import unittest

RECORD = struct.Struct('!4sB16sQH')
CRED = struct.Struct('3i')
MAGIC, VERSION, OVERFLOW = b'LXS1', 1, 1


class MockOwner:
    def __init__(self):
        self.current = None
        self.status = 'unknown'
        self.event = None

    def begin(self, generation, expected_pid, trusted_direct_exec):
        self.current = (generation, expected_pid, trusted_direct_exec)
        self.status, self.event = 'unknown', None

    def receive(self, sock):
        data, anc, flags, _ = sock.recvmsg(RECORD.size + 1, socket.CMSG_SPACE(CRED.size))
        if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC) or len(data) != RECORD.size:
            return False
        creds = [CRED.unpack(value[:CRED.size]) for level, kind, value in anc
                 if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS and len(value) >= CRED.size]
        if len(creds) != 1 or self.current is None:
            return False
        magic, version, generation, seq, code = RECORD.unpack(data)
        active, pid, trust = self.current
        if not trust or creds[0][0] != pid or generation != active:
            return False
        if (magic, version, seq, code) != (MAGIC, VERSION, 1, OVERFLOW) or self.event is not None:
            return False
        self.status, self.event = 'capture_overflow', (generation, seq)
        return True

    def end(self, generation):
        if self.current and self.current[0] == generation:
            self.current = None

    def ack(self, event):
        if event != self.event or event is None:
            return False
        self.event = None
        self.status = 'unknown'
        return True


def packet(generation, seq=1, code=OVERFLOW):
    return RECORD.pack(MAGIC, VERSION, generation, seq, code)


@unittest.skipUnless(hasattr(socket, 'SO_PASSCRED') and hasattr(os, 'fork'), 'Linux credentials required')
class ChannelContract(unittest.TestCase):
    def setUp(self):
        self.rx, self.tx = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.rx.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
        self.rx.settimeout(2)
        self.owner = MockOwner()
        self.gen = os.urandom(16)

    def tearDown(self):
        self.rx.close()
        self.tx.close()

    def send(self, data):
        self.tx.send(data)
        return self.owner.receive(self.rx)

    def test_authenticated_fixture_and_nonconsuming_ack(self):
        self.owner.begin(self.gen, os.getpid(), True)
        self.assertTrue(self.send(packet(self.gen)))
        event = self.owner.event
        self.assertEqual(self.owner.status, 'capture_overflow')
        self.assertFalse(self.send(packet(self.gen)))
        self.assertEqual(self.owner.event, event)
        self.owner.end(self.gen)
        self.assertEqual(self.owner.status, 'capture_overflow')
        self.assertTrue(self.owner.ack(event))
        self.assertEqual(self.owner.status, 'unknown')

    def test_generic_script_even_same_pid_and_exit_75_is_unknown(self):
        self.owner.begin(self.gen, os.getpid(), False)
        self.assertFalse(self.send(packet(self.gen)))
        script_exit = 75
        self.assertEqual(script_exit, 75)
        self.assertEqual(self.owner.status, 'unknown')

    def test_forwarded_descriptor_from_different_child_is_rejected(self):
        self.owner.begin(self.gen, os.getpid(), True)
        pid = os.fork()
        if pid == 0:
            try:
                self.tx.send(packet(self.gen))
            finally:
                os._exit(0)
        try:
            self.assertFalse(self.owner.receive(self.rx))
            self.assertEqual(self.owner.status, 'unknown')
        finally:
            _, status = os.waitpid(pid, 0)
            self.assertEqual(status, 0)
        self.assertTrue(self.send(packet(self.gen)))

    def test_stale_record_end_and_ack_cannot_affect_new_session(self):
        self.owner.begin(self.gen, os.getpid(), True)
        self.assertTrue(self.send(packet(self.gen)))
        old_event = self.owner.event
        new = os.urandom(16)
        self.assertNotEqual(new, self.gen)
        self.owner.begin(new, os.getpid(), True)
        self.assertFalse(self.send(packet(self.gen)))
        self.owner.end(self.gen)
        self.assertTrue(self.send(packet(new)))
        self.assertFalse(self.owner.ack(old_event))
        self.assertEqual(self.owner.event, (new, 1))

    def test_malformed_unknown_and_late_fail_closed(self):
        self.owner.begin(self.gen, os.getpid(), True)
        for data in (b'', packet(self.gen)[:-1], packet(self.gen)+b'x',
                     b'BAD!' + packet(self.gen)[4:], packet(self.gen, seq=2),
                     packet(self.gen, code=2), packet(os.urandom(16))):
            self.assertFalse(self.send(data))
            self.assertEqual(self.owner.status, 'unknown')
        self.owner.end(self.gen)
        self.assertFalse(self.send(packet(self.gen)))
        self.assertEqual(self.owner.status, 'unknown')


if __name__ == '__main__':
    unittest.main(verbosity=2)
