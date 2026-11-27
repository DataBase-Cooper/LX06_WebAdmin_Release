"""Broker ownership/lifecycle fixtures. Synthetic executable, no SDK/auth/cloud."""
import argparse,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parents[1];ROOT=HERE.parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 assert p.returncode==0,(args,p.stdout[-1500:],p.stderr[-3000:])
 return p.stdout
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--output',required=True);args=ap.parse_args();out=Path(args.output).resolve()
 assert out.parent==(ROOT/'build').resolve() and not out.exists();out.mkdir()
 paths=[HERE/'bootstrap.c',HERE/'bootstrap.h',HERE/'io/compat.h',Path(__file__).with_suffix('.c'),Path(__file__),HERE/'tests/bootstrap_mock.c']
 report=dict(passed=False,device_access=False,cloud_requests=False,sdk_calls=False,credential_files_read=False,sources={p.relative_to(ROOT).as_posix():sha(p) for p in paths})
 mock=out/'bootstrap-mock';run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-static',HERE/'tests/bootstrap_mock.c','-o',mock]);mock.chmod(0o755)
 source=[HERE/'bootstrap.c',Path(__file__).with_suffix('.c')];host=out/'bootstrap-host'
 run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all',*source,'-o',host])
 # DrvFS here reports generated files0777 even after chmod. Keep strict
 # product executable permissions and move only the synthetic fixture to tmpfs.
 with tempfile.TemporaryDirectory(prefix='dc-boot-host-') as temporary:
  trusted_mock=Path(temporary)/'broker';shutil.copyfile(mock,trusted_mock);trusted_mock.chmod(0o755)
  report['host']=run([host,trusted_mock]).strip();assert report['host']=='bootstrap cases=13 passed'
 objects=[]
 for i,p in enumerate([*source,ROOT/'hooks/webd/shim225.c']):
  obj=out/(str(i)+'.o');extra=['-D_POSIX_C_SOURCE=200809L'] if p.name=='shim225.c' else []
  run(['arm-linux-gnueabihf-gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread','-D_TIME_BITS=32','-fPIE',*extra,'-c',p,'-o',obj]);objects.append(obj)
 legacy=Path(os.environ.get('LX06_LEGACY_LIB','/opt/lx06/legacy'));crt=Path('/usr/arm-linux-gnueabihf/lib');gcc=Path(run(['arm-linux-gnueabihf-gcc','-print-file-name=crtbeginS.o']).strip()).parent
 arm=out/'bootstrap-arm';run(['arm-linux-gnueabihf-gcc','-nostdlib','-pie','-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3','-Wl,-z,relro,-z,now','-Wl,--unresolved-symbols=ignore-in-shared-libs',crt/'Scrt1.o',crt/'crti.o',gcc/'crtbeginS.o',*objects,legacy/'sysarm-libpthread.so.0',legacy/'sysarm-libc-2.25.so',legacy/'sysarm-libgcc_s.so.1','-lgcc',gcc/'crtendS.o',crt/'crtn.o','-o',arm])
 assert os.readlink('/proc/self/ns/mnt')!=os.readlink('/proc/1/ns/mnt'),'run in unshare --mount --net'
 run(['mount','--make-rprivate','/'])
 with tempfile.TemporaryDirectory(prefix='dc-boot-arm-') as temporary:
  tree=Path(temporary)/'root';run(['unsquashfs','-no-progress','-d',tree,ROOT/'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'])
  # Match the real device's tmpfs. Host overlay directory inode values exceed
  # the legacy 32-bit readdir ABI and are not representative of LX06 /tmp.
  run(['mount','-t','tmpfs','-o','mode=1777,size=16m','tmpfs',tree/'tmp'])
  for origin,target in [(Path(shutil.which('qemu-arm-static')),tree/'qemu-arm-static'),(arm,tree/'tmp/bootstrap-arm'),(mock,tree/'tmp/bootstrap-mock')]:shutil.copyfile(origin,target);target.chmod(0o755)
  (tree/'proc').mkdir(exist_ok=True);(tree/'dev').mkdir(exist_ok=True);null=tree/'dev/null'
  if null.exists() or null.is_symlink():null.unlink()
  null.touch();run(['mount','--bind','/proc',tree/'proc']);run(['mount','--bind','/dev/null',null])
  try:
   # This QEMU's PR_GET_CHILD_SUBREAPER wrapper forwards an untranslated guest
   # pointer and returns EFAULT. Do not weaken the production check to make a
   # fixture pass. Stock-ABI testing here covers external lifecycle only;
   # managed lifecycle is separately proven on the real host kernel above.
   report['stock_arm']=run(['chroot',tree,'/qemu-arm-static','/tmp/bootstrap-arm','/tmp/bootstrap-mock','--external-only']).strip();assert report['stock_arm']=='bootstrap cases=3 passed'
   report['stock_arm_managed_lifecycle']=False
   report['stock_arm_managed_limitation']='QEMU PR_GET_CHILD_SUBREAPER untranslated output pointer: EFAULT; production check retained'
   report['stock_arm_scanner']=False
   report['stock_arm_scanner_limitation']='Synthetic comm scanner returned ESRCH under stock-glibc ARM/QEMU; unresolved, not accepted'
  finally:run(['umount',null]);run(['umount',tree/'proc']);run(['umount',tree/'tmp'])
 report['source_unchanged']=all(sha(ROOT/p)==digest for p,digest in report['sources'].items());assert report['source_unchanged']
 report['binaries']={p.name:dict(sha256=sha(p),bytes=p.stat().st_size) for p in (host,arm,mock)};report['passed']=True
 (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('sources','binaries')}))
if __name__=='__main__':main()
