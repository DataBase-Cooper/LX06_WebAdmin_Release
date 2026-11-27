"""Read and verify optional upload scripts without executing shell or contacting devices."""
import gzip
import hashlib
from pathlib import Path
import re


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_scripts(folder: Path, expected: dict) -> dict:
    actual = {}
    for name, identity in expected.items():
        if Path(name).name != name or not name.endswith('.sh'):
            raise ValueError('invalid companion script filename')
        path = folder / name
        data = path.read_bytes()
        if b'\r' in data:
            raise ValueError(f'CRLF in companion script: {name}')
        if sha256(path) != identity['sha256']:
            raise ValueError(f'companion script hash mismatch: {name}')
        # Both tested installers store gzip bytes as octal printf chunks. Parse
        # data only: never run an uploaded script to check its embedded client.
        chunks = re.findall(rb"^\s*printf '((?:\\[0-7]{3})+)' >> (.+)$", data, re.M)
        if not chunks or len({target for _, target in chunks}) != 1:
            raise ValueError(f'unknown embedded payload format: {name}')
        compressed = bytes(int(token, 8) for chunk, _ in chunks
                           for token in re.findall(rb'\\([0-7]{3})', chunk))
        client = gzip.decompress(compressed)
        client_hash = hashlib.sha256(client).hexdigest()
        if not client.startswith(b'\x7fELF') or client_hash != identity['client_sha256']:
            raise ValueError(f'embedded client differs from tested binary: {name}')
        actual[name] = {'sha256': sha256(path), 'bytes': len(data),
                        'client_sha256': client_hash, 'client_bytes': len(client)}
    return actual
