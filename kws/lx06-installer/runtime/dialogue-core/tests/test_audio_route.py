"""Pure queue/identity tests, host ASan/UBSan and stock-glibc ARM. No device/cloud."""
import argparse,hashlib,json,os,shutil,subprocess,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parents[1];ROOT=HERE.parent.parent
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(args):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True)
 assert p.returncode==0,(args,p.stdout[-2000:],p.stderr[-3000:])
 return p.stdout
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--output',required=True);args=parser.parse_args()
 output=Path(args.output).resolve();assert output.parent==(ROOT/'build').resolve() and not output.exists();output.mkdir()
 paths=[HERE/'audio_route.c',HERE/'audio_route.h',HERE/'types.h',Path(__file__).with_suffix('.c'),Path(__file__)]
 report=dict(passed=False,device_access=False,cloud_requests=False,actual_audio=False,sources={p.relative_to(ROOT).as_posix():sha(p) for p in paths})
 c=[HERE/'audio_route.c',Path(__file__).with_suffix('.c')]
 host=output/'route-host';run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',*c,'-o',host])
 report['host']=run([host]).strip();assert report['host']=='audio-route cases=12 passed'
 objects=[]
 for i,path in enumerate([*c,ROOT/'hooks/webd/shim225.c']):
  obj=output/(str(i)+'.o');run(['arm-linux-gnueabihf-gcc','-std=c11','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L','-O2','-fPIE','-D_TIME_BITS=32','-c',path,'-o',obj]);objects.append(obj)
 legacy=Path(os.environ.get('LX06_LEGACY_LIB','/opt/lx06/legacy'));crt=Path('/usr/arm-linux-gnueabihf/lib')
 gcc=Path(run(['arm-linux-gnueabihf-gcc','-print-file-name=crtbeginS.o']).strip()).parent;arm=output/'route-arm'
 run(['arm-linux-gnueabihf-gcc','-nostdlib','-pie','-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3','-Wl,-z,relro,-z,now',crt/'Scrt1.o',crt/'crti.o',gcc/'crtbeginS.o',*objects,legacy/'sysarm-libc-2.25.so',legacy/'sysarm-libgcc_s.so.1','-lgcc',gcc/'crtendS.o',crt/'crtn.o','-o',arm])
 with tempfile.TemporaryDirectory(prefix='dc-route-') as temporary:
  tree=Path(temporary)/'root';run(['unsquashfs','-no-progress','-d',tree,ROOT/'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'])
  shutil.copyfile(shutil.which('qemu-arm-static'),tree/'qemu-arm-static');(tree/'qemu-arm-static').chmod(0o755)
  shutil.copyfile(arm,tree/'tmp/route');(tree/'tmp/route').chmod(0o755)
  report['stock_arm']=run(['chroot',tree,'/qemu-arm-static','/tmp/route']).strip();assert report['stock_arm']=='audio-route cases=12 passed'
 report['binaries']={p.name:dict(sha256=sha(p),bytes=p.stat().st_size) for p in (host,arm)}
 report['source_unchanged']=all(sha(ROOT/p)==value for p,value in report['sources'].items());assert report['source_unchanged']
 report['passed']=True;(output/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('sources','binaries')}))
if __name__=='__main__':main()
