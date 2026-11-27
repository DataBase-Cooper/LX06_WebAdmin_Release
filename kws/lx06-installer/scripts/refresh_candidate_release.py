"""Refresh checksums and local ZIPs for an unpublished candidate directory."""
from __future__ import annotations
import argparse, hashlib, json, os, zipfile
from pathlib import Path

def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def refresh(folder):
    version=json.loads((folder/'official-ssh/firmware.json').read_text('utf8'))['version']
    assert folder.name.endswith(version), 'version mismatch'
    files={p.relative_to(folder).as_posix():{'sha256':sha(p),'bytes':p.stat().st_size}
           for p in sorted(folder.rglob('*')) if p.is_file() and p.name!='文件校验.json'}
    manifest={'version':version,'user_scripts_included':True,'acceptance_complete':False,'files':files}
    (folder/'文件校验.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n','utf8')
    target=folder.parent/(folder.name+'.zip');temp=target.with_suffix('.refresh.tmp')
    assert not temp.exists()
    with zipfile.ZipFile(temp,'w',zipfile.ZIP_DEFLATED) as archive:
        for name in [*files,'文件校验.json']:archive.write(folder/name,name)
    with zipfile.ZipFile(temp) as archive:
        assert archive.testzip() is None and set(archive.namelist())==set(files)|{'文件校验.json'}
        for name,item in files.items():
            with archive.open(name) as stream:assert hashlib.file_digest(stream,'sha256').hexdigest()==item['sha256'],name
    temp.replace(target)
    return {'name':target.name,'sha256':sha(target),'bytes':target.stat().st_size,'members':len(files)+1}

def main():
    parser=argparse.ArgumentParser();parser.add_argument('folders',nargs='+',type=Path);args=parser.parse_args()
    results=[refresh(path) for path in args.folders]
    print(json.dumps(results,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
