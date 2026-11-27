"""Test-only ownership contract; synthetic feed, not production capture or ASR."""
import unittest
from collections import deque
from enum import Enum


class Result(Enum):
    ACCEPTED = 1
    HELD_BACKPRESSURE = 2
    REJECTED_BACKPRESSURE = 3
    REJECTED_STOPPED = 4


class Prototype:
    """Single-threaded model: caller submits in order; one frame may be held.

    ACCEPTED and HELD transfer ownership; REJECTED does not. The overflow
    incident remains latched until stop; draining never makes it disappear.
    """
    def __init__(self, capacity, max_frame):
        self.capacity = capacity
        self.max_frame = max_frame
        self.pending = deque()
        self.pending_bytes = 0
        self.held = None
        self.overflow = False
        self.rejected_frames = 0
        self.rejected_bytes = 0
        self.stopping = False

    def offer(self, frame):
        if self.stopping:
            return Result.REJECTED_STOPPED
        if not 0 < len(frame) <= self.max_frame or len(frame) > self.capacity:
            raise ValueError('invalid frame')
        if self.held is not None:
            self.rejected_frames += 1
            self.rejected_bytes += len(frame)
            return Result.REJECTED_BACKPRESSURE
        frame = bytes(frame)  # capture stack storage is borrowed
        if len(frame) > self.capacity - self.pending_bytes:
            self.held = frame
            self.overflow = True
            return Result.HELD_BACKPRESSURE
        self.pending.append(frame)
        self.pending_bytes += len(frame)
        return Result.ACCEPTED

    def drain_one(self):
        if not self.pending:
            return None
        frame = self.pending.popleft()
        self.pending_bytes -= len(frame)
        if self.held is not None and len(self.held) <= self.capacity - self.pending_bytes:
            self.pending.append(self.held)
            self.pending_bytes += len(self.held)
            self.held = None
        return frame

    def stop(self):
        # Close admission, retain all accepted data until explicitly drained.
        self.stopping = True


class BackpressureTests(unittest.TestCase):
    def test_sustained_overflow_late_flush_and_stop(self):
        q = Prototype(128000, 960)  # 4 s mono 16 kHz s16le, plus one frame
        accepted = []
        for i in range(400):
            frame = bytearray((i % 251 for _ in range(320)))
            self.assertEqual(q.offer(frame), Result.ACCEPTED)
            accepted.append(bytes(frame))
        held = bytearray([252] * 960)
        self.assertEqual(q.offer(held), Result.HELD_BACKPRESSURE)
        accepted.append(bytes(held))
        held[:] = b'\x00' * len(held)  # producer reuses its borrowed buffer
        for i in range(125):
            self.assertEqual(q.offer(bytes([i]) * 320), Result.REJECTED_BACKPRESSURE)
        self.assertEqual((q.rejected_frames, q.rejected_bytes), (125, 40000))
        self.assertTrue(q.overflow)
        self.assertEqual(q.pending_bytes, 128000)
        self.assertEqual(len(q.held), 960)
        q.stop()
        self.assertEqual(q.offer(b'new'), Result.REJECTED_STOPPED)
        delivered = []
        while q.pending:
            delivered.append(q.drain_one())
        self.assertIsNone(q.held)
        self.assertEqual(delivered, accepted)
        self.assertTrue(q.overflow)  # late flush is not incident erasure
        self.assertEqual((q.rejected_frames, q.rejected_bytes), (125, 40000))

    def test_partial_drain_then_retry_in_order(self):
        q = Prototype(8, 4)
        for data in (b'aaaa', b'bbbb'):
            self.assertEqual(q.offer(data), Result.ACCEPTED)
        self.assertEqual(q.offer(b'cccc'), Result.HELD_BACKPRESSURE)
        self.assertEqual(q.offer(b'dddd'), Result.REJECTED_BACKPRESSURE)
        self.assertEqual(q.drain_one(), b'aaaa')
        # The old held frame moved into pending, freeing the held slot.
        self.assertEqual(q.offer(b'dddd'), Result.HELD_BACKPRESSURE)
        self.assertEqual(q.drain_one(), b'bbbb')
        self.assertEqual([q.drain_one(), q.drain_one()], [b'cccc', b'dddd'])
        self.assertIsNone(q.drain_one())
        self.assertEqual(q.rejected_frames, 1)  # attempts, not unique samples

    def test_stop_with_held_frame_and_variable_sizes(self):
        q = Prototype(8, 6)
        for x in (b'123456', b'78'):
            self.assertEqual(q.offer(x), Result.ACCEPTED)
        self.assertEqual(q.offer(b'abcdef'), Result.HELD_BACKPRESSURE)
        q.stop()
        self.assertEqual(q.offer(b'x'), Result.REJECTED_STOPPED)
        self.assertEqual([q.drain_one(), q.drain_one(), q.drain_one()],
                         [b'123456', b'78', b'abcdef'])
        self.assertEqual(q.pending_bytes, 0)
        self.assertIsNone(q.held)

    def test_invalid_frames_are_not_accepted(self):
        q = Prototype(8, 6)
        for frame in (b'', b'1234567'):
            with self.assertRaises(ValueError):
                q.offer(frame)
        self.assertFalse(q.overflow)
        self.assertEqual(q.pending_bytes, 0)


if __name__ == '__main__':
    unittest.main()
