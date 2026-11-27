"""Offline metadata-only counterexample; no device, network, audio files or PCM.

The hidden physical times below are fixture ground truth, never observation
fields. This does not run the production capture, player, or guard.
"""
import unittest
from dataclasses import dataclass


@dataclass(frozen=True)
class Observation:
    accepted_ms: int
    accepted_frames: int
    available_ms: int
    receiver_ms: int
    loop_ms: int
    sequence: int
    flags: int
    guard_echo: bool
    guard_voiced: bool


def observations(hidden_capture_ms, hidden_playout_ms):
    """Two different hidden histories may expose precisely the same metadata."""
    assert hidden_capture_ms <= 1100 and hidden_playout_ms >= 1000
    return Observation(1000, 160, 1100, 1110, 1105, 42, 0, True, True)


class AlignmentObservability(unittest.TestCase):
    def test_identical_metadata_does_not_determine_acoustic_alignment(self):
        # Unknown hardware, transport and processing delays allow both worlds.
        a = (1050, 1020)  # capture 30 ms after playback
        b = (950, 1090)   # capture 140 ms before playback
        self.assertEqual(observations(*a), observations(*b))
        self.assertNotEqual(a[0] - a[1], b[0] - b[1])

    def test_receiver_latency_is_not_capture_age(self):
        o = observations(950, 1090)
        self.assertEqual(o.receiver_ms - o.available_ms, 10)
        self.assertEqual(o.receiver_ms - 950, 160)
        self.assertNotEqual(o.receiver_ms - o.available_ms, o.receiver_ms - 950)

    def test_reactor_snapshot_can_precede_receive(self):
        o = observations(1050, 1020)
        self.assertLess(o.loop_ms, o.receiver_ms)


if __name__ == '__main__':
    unittest.main(verbosity=2)
