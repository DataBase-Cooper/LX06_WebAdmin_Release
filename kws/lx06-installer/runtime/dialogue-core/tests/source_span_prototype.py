"""Abstract, offline source ledger. Inputs are assumed certified, not detected.

No decoder, transport, capture or physical audio is implemented here.
"""
from dataclasses import dataclass


@dataclass(frozen=True)
class Span:
    pcm: bytes
    generation: int | None
    utterance: int | None
    voiced: bool | None


class Queue:
    def __init__(self, capacity):
        self.capacity = capacity
        self.spans = []
        self.size = 0

    def append(self, span):
        if not span.pcm:
            raise ValueError('empty span')
        if len(span.pcm) > self.capacity - self.size:
            raise BufferError('source queue full; no bytes discarded')
        self.spans.append(span)
        self.size += len(span.pcm)

    def decoder_final(self):
        # A final concerns the old decoder, not the pending capture queue.
        pass

    def pop_utterance(self):
        if not self.spans:
            return []
        take = 1
        # A missing boundary cannot prove where to split: conservatively absorb
        # the remaining available queue; caller must report ambiguity.
        unknown = self.spans[0].utterance is None
        while take < len(self.spans):
            previous, current = self.spans[take - 1], self.spans[take]
            if current.utterance is None:
                unknown = True
            if not unknown and previous.utterance != current.utterance:
                break
            take += 1
        result, self.spans = self.spans[:take], self.spans[take:]
        self.size -= sum(len(s.pcm) for s in result)
        return result


def decide(spans, current):
    if not spans or not current or any(s.utterance is None or s.voiced is None for s in spans):
        return 'ambiguous'
    if len({s.utterance for s in spans}) != 1:
        return 'ambiguous'
    owners = {s.generation for s in spans if s.voiced}
    if not owners or None in owners or 0 in owners or len(owners) != 1:
        return 'ambiguous'
    return 'current' if next(iter(owners)) == current else 'stale'
