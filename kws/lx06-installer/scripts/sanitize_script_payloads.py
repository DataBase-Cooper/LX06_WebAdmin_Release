"""Redact connection defaults inside generated gzip clients, without execution.

Keep ELF offsets/size intact: replace only known URL literals with a shorter
invalid endpoint plus NUL padding. The script's explicit user URL is required.
Update every outer SHA256 reference and rebuild the literal octal gzip payload.
Other credential types are rejected, never patched heuristically in binaries.
"""
import gzip,hashlib,io,re
PRINTF=rb"printf[ \t]+'((?:\\[0-3][0-7]{2})+)'[ \t]*>>[ \t]*\"([^\"\r\n]+[.]gz)\""

def payload_groups(data):
    groups={}
    for match in re.finditer(PRINTF,data):
        raw=bytes(int(v,8) for v in re.findall(rb'\\([0-3][0-7]{2})',match[1]))
        groups.setdefault(match[2],bytearray()).extend(raw)
    return groups

def decode(payload):
    with gzip.GzipFile(fileobj=io.BytesIO(payload)) as stream:raw=stream.read(8*1024*1024+1)
    if len(raw)>8*1024*1024:raise ValueError('oversize embedded client')
    return raw

def sanitize_payloads(data,values):
    replacements={};hashes={}
    for target,payload in payload_groups(data).items():
        original=decode(payload);raw=original
        for secret in values:
            if secret not in raw:continue
            substitute=b'wss://invalid.invalid/' if secret.startswith(b'wss://') else b'ws://invalid.invalid/'
            if not secret.startswith((b'wss://',b'ws://')) or len(secret)<len(substitute):
                raise ValueError('embedded client contains a non-URL private value')
            raw=raw.replace(secret,substitute+b'\0'*(len(secret)-len(substitute)))
        if raw==original:continue
        oldsha=hashlib.sha256(original).hexdigest().encode();newsha=hashlib.sha256(raw).hexdigest().encode()
        if oldsha not in data:raise ValueError('missing embedded client checksum reference')
        assert len(raw)==len(original)
        packed=gzip.compress(raw,9,mtime=0)
        lines=[]
        for offset in range(0,len(packed),128):
            octal=b''.join(('\\%03o'%v).encode() for v in packed[offset:offset+128])
            lines.append(b"printf '"+octal+b"' >> \""+target+b'"')
        replacements[target]=b'\n'.join(lines)
        hashes[oldsha]=newsha
    emitted=set()
    def replace(m):
        target=m[2]
        if target not in replacements:return m[0]
        if target in emitted:return b''
        emitted.add(target);return replacements[target]
    result=re.sub(PRINTF,replace,data)
    for old,new in hashes.items():result=result.replace(old,new)
    # Old generated comments list compressed byte counts. Do not retain a
    # misleading size after re-encoding; the actual hash is authoritative.
    if replacements:result=re.sub(rb'payload: ([^\r\n]+?) \(gz [0-9]+ B -> raw [0-9]+ B, sha256=([0-9a-f]+)\)',
                                  rb'payload: \1 (public endpoint placeholder; sha256=\2)',result)
    return result,len(replacements)
