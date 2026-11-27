"""Copy reviewed protocol-only project components; never modify their origins."""
import hashlib,json,re
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
XZ=ROOT.parent/'scratch/xz-client-rewrite/src'
DB=ROOT/'runtime/doubao/src'
delta_manifest=HERE/'support-local-deltas.json'
if delta_manifest.exists() and json.loads(delta_manifest.read_text()).get('files'):
 raise SystemExit('refuse to overwrite reviewed support local deltas; preserve and rebase them explicitly')
def digest(raw):return hashlib.sha256(raw).hexdigest()
rows=[]
def copy(source,target,namespace=None):
 raw=source.read_bytes();out=raw
 if namespace:
  text=raw.decode().replace('xz_ws',namespace+'_ws').replace('XZ_WS',namespace.upper()+'_WS')
  out=text.encode()
 path=HERE/'support'/target;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(out)
 rows.append(dict(path=path.relative_to(HERE).as_posix(),origin=source.relative_to(ROOT.parent).as_posix(),origin_sha256=digest(raw),sha256=digest(out),bytes=len(out),namespace_only=namespace is not None))
for name in ('json.c','json.h','ogg_audio.c','ogg_audio.h'):copy(DB/name,'common/'+name)
for name in ('protocol.c','protocol.h','event.c','event.h','session.h','opus_codec.c','opus_codec.h'):copy(XZ/name,'xiaozhi/'+name)
for label,source,prefix in (('xiaozhi',XZ,'dc_xz'),('doubao',DB,'dc_db')):
 for name in ('ws.c','ws.h','ws_frame.c','ws_frame.h'):copy(source/name,label+'/'+name,prefix)
(HERE/'support-origin.json').write_text(json.dumps(dict(schema=1,purpose='protocol-only components; no microphone/player/control-word state',files=rows),indent=2)+'\n')
print(json.dumps(dict(copied=len(rows),originals_modified=False)))
