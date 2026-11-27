"""Offline: real session-run and production-main core with injected capture ENOBUFS.

Requires Linux, isolated network/mount namespaces and POSIX scratch tmpfs. This
observes executable identity while the child is stopped, not an authenticated
production status transport; no runtime or user-visible status is added.
"""
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
CORE = HERE.parent
ROOT = CORE.parents[1]
sys.path.insert(0, str(CORE))
from build import sources
from test_capture_exit import WRAPPED


def wait_for(fn, label):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.01)
    raise AssertionError('timeout: ' + label)


def main():
    assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
    with tempfile.TemporaryDirectory(prefix='dc-attr-') as tmp:
        temp = Path(tmp)
        lease, supervisor, core = (temp / x for x in ('lease', 'session-run', 'core'))
        original = (HERE / 'test_capture_exit_wrap.c').read_text()
        target = 'if(delivered++)return 0;if(getenv("TEST_GENERIC"))'
        assert original.count(target) == 1
        wrapped = temp / 'capture-wrap.c'
        # Absolute includes preserve production fixture definitions; SIGSTOP gives
        # the observer time to attest /proc/<direct-child>/exe and birth tick.
        original = original.replace('#include "../', '#include "' + str(CORE) + '/')
        original = original.replace(target, 'if(delivered++)return 0;raise(SIGSTOP);if(getenv("TEST_GENERIC"))')
        wrapped.write_text('#include <signal.h>\n' + original)
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',
                        '-DLX06_SESSION_FILE="'+str(lease)+'"',
                        str(ROOT/'hooks/xiaoai/session-run.c'),'-o',str(supervisor)], check=True)
        subprocess.run(['gcc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-DDC_CORE_HOST_FIXTURE','-pthread','-I'+str(CORE/'vendor/libfvad/include'),
                        '-I'+str(CORE/'vendor/libfvad/src'),*map(str,sources()),str(wrapped),
                        *['-Wl,--wrap='+name for name in WRAPPED],'-ldl','-lopus','-lm','-o',str(core)],check=True)
        env = dict(os.environ, DC_HOST_FIXTURE='1', DC_TRANSPORT_ADAPTER='/bin/true', DB_GREETING='')
        env.pop('TEST_GENERIC',None); env.pop('TEST_ROUTE_OTHER',None)
        script = temp/'generic.sh'
        script.write_text('#!/bin/sh\nexit 75\n'); script.chmod(0o700)
        generic = subprocess.run([supervisor,'run','generic',script],env=env,capture_output=True,text=True,timeout=10)
        assert generic.returncode == 75, (generic.returncode,generic.stderr)
        assert 'event=capture_overflow' not in generic.stderr
        assert lease.stat().st_size == 0
        p = subprocess.Popen([supervisor,'run','core',core,'--audio','doubao'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        child = None
        try:
            def record():
                try:
                    data=lease.read_bytes()
                    if len(data)<32: return None
                    magic, spid, pid, fd, sticks, cticks=struct.unpack_from('=IIIIQQ',data)
                    if magic != 0x3153584c or spid != p.pid or pid < 2: return None
                    return (spid,pid,sticks,cticks)
                except (FileNotFoundError,struct.error): return None
            identity=wait_for(record,'session lease')
            child=identity[1]
            def stopped():
                try:
                    stat=Path('/proc',str(child),'stat').read_text().rsplit(')',1)[1].split()
                    return stat[0]=='T' and int(stat[19])==identity[3]
                except (FileNotFoundError,IndexError): return False
            wait_for(stopped,'core stopped at capture')
            assert os.path.samefile('/proc/%d/exe'%child,core)
            assert Path('/proc',str(child),'stat').read_text().rsplit(')',1)[1].split()[1]==str(p.pid)
            assert subprocess.run([supervisor,'busy']).returncode == 0
            os.kill(child,signal.SIGCONT)
            out,err=p.communicate(timeout=10)
            assert p.returncode == 75 and 'event=capture_overflow' in err, (p.returncode,out,err)
            assert subprocess.run([supervisor,'busy']).returncode == 1 and lease.stat().st_size==0
            assert not Path('/proc',str(child)).exists()
            stale = struct.pack('=IIIIQQ', 0x3153584c, identity[0], identity[1], 0, identity[2], identity[3])
            # A new generation gets its own lease and cannot be confused by a
            # remembered process identity; no production status seam exists.
            next_p = subprocess.Popen([supervisor,'run','next',core,'--audio','doubao'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                p = next_p
                next_identity=wait_for(record,'next session lease')
                child=next_identity[1]
                identity=next_identity
                wait_for(stopped,'next core stopped at capture')
                current=lease.read_bytes()[:32]
                # Compare process IDs AND start ticks, not status code or name.
                old_id=struct.unpack('=IIIIQQ',stale)
                new_id=struct.unpack('=IIIIQQ',current)
                assert (old_id[1],old_id[2],old_id[4],old_id[5]) != (new_id[1],new_id[2],new_id[4],new_id[5])
                assert os.path.samefile('/proc/%d/exe'%child,core)
                os.kill(child,signal.SIGCONT)
                out,err=p.communicate(timeout=10)
                assert p.returncode == 75 and 'event=capture_overflow' in err, (p.returncode,out,err)
                assert subprocess.run([supervisor,'busy']).returncode == 1 and lease.stat().st_size==0
            finally:
                if next_p.poll() is None:
                    try: os.kill(child,signal.SIGCONT)
                    except ProcessLookupError: pass
                    next_p.terminate(); next_p.wait(timeout=5)
            print('PASS generic script and attested direct production-main fixture both exit 75; injected ENOBUFS only in core, lease released; sequential lease identities differ')
            print('LIMIT observed identity before exit only; no authenticated status or stale-generation rejection transport')
        finally:
            if p.poll() is None:
                if child:
                    try: os.kill(child,signal.SIGCONT)
                    except ProcessLookupError: pass
                p.terminate()
                try: p.wait(timeout=4)
                except subprocess.TimeoutExpired: p.kill(); p.wait()


if __name__ == '__main__':
    main()
