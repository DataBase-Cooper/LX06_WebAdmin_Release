"""Update an existing PRIVATE Xiaozhi upload script without changing its defaults.

Usage: python3 scripts/build_xz_gain_update.py source.sh armhf_client output.sh
Inputs may contain credentials. Never publish the generated user script.
Only replace the embedded client, its hashes, and the gain field in the schema.
"""
import gzip
import hashlib
import json
from pathlib import Path
import re
import sys


def build(source, client, output):
    raw=client.read_bytes()
    if raw[:4]!=b'\x7fELF' or int.from_bytes(raw[18:20],'little')!=40:
        raise ValueError('Expected LX06 ARM ELF client')
    text=source.read_text(encoding='utf-8')
    digest=hashlib.sha256(raw).hexdigest()
    text,n=re.subn(r"^XZ_CLIENT_SHA256='[0-9a-f]{64}'$",f"XZ_CLIENT_SHA256='{digest}'",text,flags=re.M)
    assert n==1
    compressed=gzip.compress(raw,9,mtime=0)
    header=f'  # ---- payload: xz_client (gz {len(compressed)} B -> raw {len(raw)} B, sha256={digest}) ----'
    chunks=[''.join('\\%03o'%b for b in compressed[i:i+128]) for i in range(0,len(compressed),128)]
    lines=[header,'rm -f "$WORK/xz_client.gz"; : > "$WORK/xz_client.gz"']
    lines += ["printf '"+chunk+"' >> \"$WORK/xz_client.gz\"" for chunk in chunks]
    pattern=r'^[ \t]*# ---- payload: xz_client[^\n]*\n[^\n]*\n(?:[ \t]*printf \'(?:\\[0-7]{3})+\' >> [^\n]*\n)+'
    text,n=re.subn(pattern,lambda _: '\n'.join(lines)+'\n',text,flags=re.M)
    assert n==1, 'Unknown payload layout; refusing partial update'
    schema_pattern=r'(# LX06-EXTENSION-BEGIN\n# )([^\n]+)(\n# LX06-EXTENSION-END)'
    def update_schema(match):
        schema=json.loads(match[2])
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime/model-adapter'))
        from voice_settings import schema as current_schema
        gain=next(f for f in current_schema('xiaozhi')['fields'] if f['name']=='XZ_INPUT_GAIN')
        schema['fields']=[f for f in schema['fields'] if f['name']!='XZ_INPUT_GAIN']
        schema['fields'].insert(0,gain)
        for action in schema['actions']:
            if action['id']=='save' and 'XZ_INPUT_GAIN' not in action['fields']:
                action['fields'].insert(0,'XZ_INPUT_GAIN')
        return match[1]+json.dumps(schema,ensure_ascii=False,separators=(',',':'))+match[3]
    text,n=re.subn(schema_pattern,update_schema,text)
    assert n==1
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(text,encoding='utf-8',newline='\n')
    from release_payloads import verify_scripts
    checked=verify_scripts(output.parent,{output.name:{'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'client_sha256':digest}})
    print(json.dumps(checked,ensure_ascii=False))


if __name__=='__main__': build(*(Path(p) for p in sys.argv[1:]))
