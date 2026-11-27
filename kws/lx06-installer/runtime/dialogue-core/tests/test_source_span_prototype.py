"""Offline abstract source-span contract; not a production ASR test."""
import unittest
from source_span_prototype import Queue, Span, decide


class SourceSpanTests(unittest.TestCase):
    def test_final_wait_preserves_two_distinct_sentences(self):
        q = Queue(100)
        q.append(Span(b'old', 1, 10, True))
        q.decoder_final()
        q.append(Span(b'new', 2, 11, True))
        old = q.pop_utterance()
        self.assertEqual(b''.join(s.pcm for s in old), b'old')
        self.assertEqual(decide(old, 2), 'stale')
        self.assertEqual(len(q.spans), 1)
        new = q.pop_utterance()
        self.assertEqual(b''.join(s.pcm for s in new), b'new')
        self.assertEqual(decide(new, 2), 'current')

    def test_mixed_generation_utterance_is_ambiguous(self):
        q = Queue(100)
        q.append(Span(b'a', 1, 12, True))
        q.append(Span(b'b', 2, 12, True))
        q.append(Span(b'next', 2, 13, True))
        self.assertEqual(decide(q.pop_utterance(), 2), 'ambiguous')
        self.assertEqual(decide(q.pop_utterance(), 2), 'current')

    def test_unknown_speech_and_unknown_boundary_fail_closed(self):
        self.assertEqual(decide([Span(b'x', None, 4, True)], 2), 'ambiguous')
        self.assertEqual(decide([Span(b'x', 2, None, True)], 2), 'ambiguous')
        self.assertEqual(decide([Span(b'x', 2, 4, None)], 2), 'ambiguous')

    def test_silence_cannot_establish_identity(self):
        self.assertEqual(decide([Span(b'x', 2, 4, False)], 2), 'ambiguous')
        self.assertEqual(decide([Span(b'x', 1, 4, False), Span(b'y', 2, 4, True)], 2), 'current')

    def test_stale_and_ambiguous_cannot_interrupt(self):
        stops = []
        def control(spans):
            if decide(spans, 2) == 'current':
                stops.append(2)
        control([Span(b'old', 1, 10, True)])
        control([Span(b'mixed1', 1, 11, True), Span(b'mixed2', 2, 11, True)])
        control([Span(b'unknown', None, 12, True)])
        self.assertEqual(stops, [])
        control([Span(b'new', 2, 13, True)])
        self.assertEqual(stops, [2])

    def test_capacity_failure_is_atomic_not_truncation(self):
        q = Queue(3)
        q.append(Span(b'ab', 1, 1, True))
        with self.assertRaises(BufferError):
            q.append(Span(b'cd', 2, 2, True))
        self.assertEqual(q.pop_utterance()[0].pcm, b'ab')

    def test_unknown_boundary_does_not_split_from_next(self):
        q = Queue(100)
        q.append(Span(b'a', 1, None, True))
        q.append(Span(b'b', 2, 2, True))
        self.assertEqual(decide(q.pop_utterance(), 2), 'ambiguous')
        self.assertFalse(q.spans)


if __name__ == '__main__':
    unittest.main()
