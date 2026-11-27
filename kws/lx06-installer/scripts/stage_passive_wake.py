"""Install the tested passive audio/session stack into an unpacked firmware.

Feature selection is explicit. No recordings, models or numeric thresholds are
seeded: the watcher requires a threshold calibrated for the installed feature.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import os
from patch_stock_led import patch_stock_led

ROOT=Path(__file__).resolve().parents[1]
PRELOAD='    procd_set_param env LD_PRELOAD=/usr/lib/lx06/xiaoai/processed-mic-tap.so'

def patch_pns(text):
    begin=text.index('_start_mipns_xiaomi() {')
    end=text.index('\n}',begin)
    body=text[begin:end]
    if PRELOAD in body:
        if body.count(PRELOAD)!=1: raise ValueError('duplicate native preload')
        return text
    if 'LD_PRELOAD=' in body: raise ValueError('unexpected preload in Xiaomi instance')
    anchor='    procd_open_instance\n'
    if body.count(anchor)!=1: raise ValueError('ambiguous Xiaomi instance')
    body=body.replace(anchor,anchor+PRELOAD+'\n')
    return text[:begin]+body+text[end:]

def stage(target,feature):
    target=target.resolve()
    if target==Path('/') or not (target/'etc/init.d/pns').is_file():
        raise ValueError('expected unpacked LX06 rootfs')
    modes={'embedding-int8':('build_ww_int8.sh','wakeword_template_int8',7),
           'embedding-fp32':('build_ww.sh','wakeword_template',6),
           'mfcc':('build_ww_mfcc.sh','wakeword_template_mfcc',4),
           'mfcc-lifter32':('build_ww_mfcc.sh','wakeword_template_mfcc_robust',8)}
    build,binary,version=modes[feature]
    protected=['usr/lib/libxaudio_engine.so','etc/asound.conf','etc/asound.conf.dts']
    stock_hashes={p:hashlib.sha256((target/p).read_bytes()).hexdigest() for p in protected}
    subprocess.run(['bash',str(ROOT/'hooks/wakeword'/build)]+(['robust'] if feature=='mfcc-lifter32' else []),check=True,cwd=ROOT)
    if feature=='mfcc-lifter32':
        subprocess.run(['arm-linux-gnueabihf-gcc','-O3','-mcpu=cortex-a53','-mfpu=neon-vfpv4',
                        '-static','-ffast-math','-o',str(ROOT/'hooks/wakeword/build/wakeword_calibrate'),
                        str(ROOT/'hooks/wakeword/wakeword_calibrate.c'),'-lm'],check=True)
        subprocess.run(['arm-linux-gnueabihf-strip',str(ROOT/'hooks/wakeword/build/wakeword_calibrate')],check=True)
    subprocess.run(['bash','scripts/build_processed_mic.sh'],cwd=ROOT,check=True,
                   env=dict(os.environ,LX06_SYSROOT_LIB=str(target/'lib')))
    subprocess.run(['bash','scripts/build_listen_window.sh'],cwd=ROOT,check=True)
    router=ROOT/'hooks/xiaoai/build/official-router'
    subprocess.run(['arm-linux-gnueabihf-gcc','-D_TIME_BITS=32','-D_FILE_OFFSET_BITS=32',
                    '-O2','-Wall','-Wextra','-static','-o',str(router),
                    str(ROOT/'hooks/xiaoai/official-router.c'),
                    str(ROOT/'runtime/doubao/src/json.c')],check=True)
    subprocess.run(['arm-linux-gnueabihf-strip',str(router)],check=True)
    files={
        'usr/lib/lx06/wakeword/wakeword_template':'hooks/wakeword/build/'+binary,
        'usr/lib/lx06/wakeword/watch-ww.sh':'hooks/wakeword/watch-ww.sh',
        'usr/lib/lx06/wakeword/ww-record.sh':'hooks/wakeword/ww-record.sh',
        'usr/lib/lx06/wakeword/ww-enroll-asr.sh':'hooks/wakeword/ww-enroll-asr.sh',
        'usr/lib/lx06/wakeword/ww-train.sh':'hooks/wakeword/ww-train.sh',
        'usr/lib/lx06/wakeword/ww-ready.wav':'hooks/wakeword/ww-ready.wav',
        'usr/lib/lx06/xiaoai/processed-mic-tap.so':'hooks/xiaoai/build/processed-mic-tap.so',
        'usr/lib/lx06/xiaoai/lx06-listen-window':'hooks/xiaoai/build/lx06-listen-window',
        'usr/lib/lx06/xiaoai/official-router':'hooks/xiaoai/build/official-router',
        'usr/lib/lx06/xiaoai/processed-mic-read':'hooks/xiaoai/build/processed-mic-read',
        'usr/lib/lx06/xiaoai/session-run':'hooks/xiaoai/build/session-run',
        'usr/bin/lx06-dialog-light':'hooks/xiaoai/build/lx06-dialog-light',
        'usr/include/lx06/dialog-light.h':'hooks/xiaoai/dialog-light.h',
        'usr/share/lx06/dialog-light-api.md':'docs/dialog-light-api.md',
        'etc/init.d/lx06wakeword':'hooks/wakeword/lx06wakeword',
        'etc/init.d/mico_aivs_lab':'hooks/xiaoai/mico_aivs_lab.init.v43',
    }
    if feature=='mfcc-lifter32':
        files['usr/lib/lx06/wakeword/wakeword_calibrate']='hooks/wakeword/build/wakeword_calibrate'
    for destination,source in files.items():
        path=target/destination
        path.parent.mkdir(parents=True,exist_ok=True)
        data=(ROOT/source).read_bytes()
        if source.endswith('.sh') or source.endswith('.v43') or '/init.d/' in destination:
            data=data.replace(b'\r\n',b'\n')
        path.write_bytes(data)
        path.chmod(0o644 if destination.endswith(('.wav','.h','.md')) else 0o755)
    # Stage the native dispatcher alongside the matching session-run build.
    # Preserve the stock shut_led symlink and the rest of the dispatcher.
    stock_led=target/'bin/show_led'
    guard=(ROOT/'hooks/xiaoai/stock-led-guard.inc').read_bytes()
    stock_led.write_bytes(patch_stock_led(stock_led.read_bytes(),guard))
    pns=target/'etc/init.d/pns'
    pns.write_text(patch_pns(pns.read_text()))
    link=target/'etc/rc.d/S62lx06wakeword'
    if link.exists() or link.is_symlink(): link.unlink()
    link.symlink_to('../init.d/lx06wakeword')
    for path,digest in stock_hashes.items():
        assert hashlib.sha256((target/path).read_bytes()).hexdigest()==digest,path
    manifest={'feature':feature,'model_version':version,
              'dialog_light_api':{'version':1,'command':'/usr/bin/lx06-dialog-light','opt_in':'LX06_DIALOG_LIGHTS=1',
                                  'ownership':'inherited per-session socket, cleanup before lease release'},
              'threshold_policy':'MFCC enrollment auto-calibrates all recordings with tolerance; manual override allowed; feature identity and exact model SHA256 must match',
              'acceptance_complete':False,
              'remaining':'full acoustic diversity, script interruptions, stability and boot acceptance',
              'stock_audio_sha256':stock_hashes,
              'files':{p:hashlib.sha256((target/p).read_bytes()).hexdigest() for p in [*files,'etc/init.d/pns','bin/show_led']}}
    (target/'usr/lib/lx06/wake-stack.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Passive wake stack staged:',feature,'model version',version,'; calibration required')

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('rootfs',type=Path)
    parser.add_argument('--feature',required=True,choices=['embedding-int8','embedding-fp32','mfcc','mfcc-lifter32'])
    args=parser.parse_args()
    stage(args.rootfs,args.feature)
