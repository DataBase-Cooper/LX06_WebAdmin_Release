"""Replace a literal generated gzip payload without executing its shell script.

Preserve user routing/defaults and update the uncompressed client's checksum.
Used by rc22 to make an upload actually contain the newly built gain client.
"""
import gzip
import hashlib
import re
from sanitize_script_payloads import PRINTF, decode, payload_groups


def replace_payload(data: bytes, target: bytes, client: bytes) -> bytes:
    groups = payload_groups(data)
    if target not in groups:
        raise ValueError('missing literal client payload')
    previous = decode(groups[target])
    old = hashlib.sha256(previous).hexdigest().encode()
    new = hashlib.sha256(client).hexdigest().encode()
    if old not in data:
        raise ValueError('missing client checksum reference')
    if previous == client:
        return data
    packed = gzip.compress(client, 9, mtime=0)
    lines = []
    for offset in range(0, len(packed), 128):
        octal = b''.join(('\\%03o' % v).encode() for v in packed[offset:offset+128])
        lines.append(b"printf '" + octal + b"' >> \"" + target + b'"')
    emitted = False

    def replacement(match):
        nonlocal emitted
        if match[2] != target:
            return match[0]
        if emitted:
            return b''
        emitted = True
        return b'\n'.join(lines)

    result = re.sub(PRINTF, replacement, data).replace(old, new)
    result = re.sub(rb'payload: xz_client[^\r\n]*',
                    b'payload: xz_client (sha256=' + new + b')', result)
    assert decode(payload_groups(result)[target]) == client
    return result
