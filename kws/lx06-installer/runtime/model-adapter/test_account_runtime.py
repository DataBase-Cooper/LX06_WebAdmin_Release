"""Execute generated shell with real gzip/hash/flock and harmless helpers."""
import concurrent.futures
import fcntl
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest
from account_runtime import build_runtime


@unittest.skipUnless(os.geteuid() == 0, 'runtime requires a root-owned TMP directory')
class RuntimeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.audio = self.root/'audio'
        self.voice = self.root/'voice'
        self.audio.write_bytes(b'#!/bin/sh\nprintf audio\n')
        self.voice.write_bytes(b'#!/bin/sh\nprintf voice\n')
        for file in (self.audio, self.voice):
            file.chmod(0o700)
        self.source = build_runtime(self.audio, self.voice)
        self.cache = self.root/'cache'
        self.source = re.sub(r'/tmp/lx06-account-speed-[a-f0-9]{16}', str(self.cache), self.source)

    def call(self, source=None, audio='/missing/audio', voice='/missing/voice', env=None):
        script = self.root/'run.sh'
        script.write_text('set -eu\numask 022\nAUDIO='+shlex.quote(str(audio))+'\nVOICE='+shlex.quote(str(voice))+'\n'+(source or self.source)+'''
account_speed_runtime
printf '%s\\n' "$AUDIO" "$VOICE" "$(umask)"
''')
        r = subprocess.run(['sh', str(script)], capture_output=True, text=True, env=env, timeout=5)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, '')
        return r.stdout.splitlines()

    def test_extract_deterministic_payload_and_reuse_without_gzip(self):
        self.assertEqual(self.source, re.sub(r'/tmp/lx06-account-speed-[a-f0-9]{16}', str(self.cache), build_runtime(self.audio, self.voice)))
        self.assertTrue(self.source.isascii())
        paths = self.call()
        self.assertEqual(paths, [str(self.cache/'model-audio'), str(self.cache/'model-voice'), '0022'])
        self.assertEqual((self.cache/'model-audio').read_bytes(), self.audio.read_bytes())
        self.assertEqual((self.cache/'model-voice').read_bytes(), self.voice.read_bytes())
        self.assertEqual(self.cache.stat().st_mode & 0o777, 0o700)
        self.assertTrue(all(p.stat().st_mode & 0o777 == (0o600 if p.name=='.lock' else 0o700) for p in self.cache.iterdir()))
        bin = self.root/'bin';bin.mkdir()
        (bin/'gzip').write_text('#!/bin/sh\necho must-not-extract >&2\nexit 1\n');(bin/'gzip').chmod(0o700)
        self.assertEqual(self.call(env={**os.environ, 'PATH':str(bin)+':'+os.environ['PATH']}), paths)
        self.assertFalse(list(self.cache.glob('.install.*')))

    def test_matching_rom_keeps_original_paths_without_tmp_creation(self):
        self.assertEqual(self.call(audio=self.audio,voice=self.voice), [str(self.audio),str(self.voice),'0022'])
        self.assertFalse(self.cache.exists())

    def test_installs_without_stat_applet(self):
        bin = self.root/'no-stat';bin.mkdir()
        marker = self.root/'stat-was-called'
        (bin/'stat').write_text('#!/bin/sh\ntouch '+shlex.quote(str(marker))+'\nexit 127\n')
        (bin/'stat').chmod(0o700)
        paths = self.call(env={**os.environ,'PATH':str(bin)+':'+os.environ['PATH']})
        self.assertEqual(paths[:2],[str(self.cache/'model-audio'),str(self.cache/'model-voice')])
        self.assertFalse(marker.exists())

    def test_optional_third_helper_is_verified_and_published_together(self):
        http=self.root/'http';http.write_bytes(b'#!/bin/sh\nprintf http\n')
        source=build_runtime(self.audio,self.voice,http)
        source=re.sub(r'/tmp/lx06-account-speed-[a-f0-9]{16}',str(self.cache),source)
        source+='\naccount_speed_runtime\n[ "$HTTP" = '+shlex.quote(str(self.cache/'model-http'))+' ]\n'
        paths=self.call(source)
        self.assertEqual(paths[:2],[str(self.cache/'model-audio'),str(self.cache/'model-voice')])
        self.assertEqual((self.cache/'model-http').read_bytes(),http.read_bytes())
        self.assertEqual((self.cache/'model-http').stat().st_mode&0o777,0o700)

    def test_unsafe_third_helper_preserves_all_original_paths(self):
        http=self.root/'http';http.write_bytes(b'#!/bin/sh\nprintf http\n')
        self.cache.mkdir(mode=0o700)
        (self.cache/'model-http').symlink_to(http)
        source=build_runtime(self.audio,self.voice,http)
        source=re.sub(r'/tmp/lx06-account-speed-[a-f0-9]{16}',str(self.cache),source)
        source='HTTP=/missing/http\n'+source+'\naccount_speed_runtime\n[ "$HTTP" = /missing/http ]\n'
        self.assertEqual(self.call(source)[:2],['/missing/audio','/missing/voice'])
        self.assertEqual(http.read_bytes(),b'#!/bin/sh\nprintf http\n')

    def test_hardlinks_unsafe_owner_and_special_modes_rejected(self):
        self.cache.mkdir(mode=0o700)
        target = self.cache/'model-audio'
        os.link(self.audio,target)
        self.assertEqual(self.call()[:2],['/missing/audio','/missing/voice'])
        target.unlink();target.write_bytes(self.audio.read_bytes());target.chmod(0o700)
        os.chown(target,65534,65534)
        self.assertEqual(self.call()[:2],['/missing/audio','/missing/voice'])
        os.chown(target,0,0);target.chmod(0o4700)
        self.assertEqual(self.call()[:2],['/missing/audio','/missing/voice'])

    def test_corrupted_payload_returns_old_paths_and_cleans_own_stage(self):
        corrupted=self.source.replace('gzip -dc >', 'gzip -dc --invalid-option >')
        self.assertEqual(self.call(corrupted), ['/missing/audio','/missing/voice','0022'])
        self.assertEqual([p.name for p in self.cache.iterdir()], ['.lock'])

    def test_unsafe_directory_lock_and_leaf_symlinks_never_followed(self):
        peer=self.root/'peer';peer.mkdir(mode=0o700)
        self.cache.symlink_to(peer, target_is_directory=True)
        self.assertEqual(self.call()[:2], ['/missing/audio','/missing/voice']);self.assertEqual(list(peer.iterdir()),[])
        self.cache.unlink();self.cache.mkdir(mode=0o755)
        self.assertEqual(self.call()[:2], ['/missing/audio','/missing/voice']);self.assertEqual(list(self.cache.iterdir()),[])
        self.cache.chmod(0o700);(self.cache/'model-audio').symlink_to(peer,target_is_directory=True)
        self.assertEqual(self.call()[:2], ['/missing/audio','/missing/voice']);self.assertEqual(list(peer.iterdir()),[])
        (self.cache/'model-audio').unlink();(self.cache/'.lock').symlink_to(self.audio)
        before=self.audio.read_bytes();self.assertEqual(self.call()[:2], ['/missing/audio','/missing/voice']);self.assertEqual(self.audio.read_bytes(),before)

    def test_busy_lock_has_immediate_fallback_and_can_install_later(self):
        self.cache.mkdir(mode=0o700)
        lock=self.cache/'.lock';lock.touch(mode=0o600)
        with lock.open('r+') as stream:
            fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.assertEqual(self.call()[:2], ['/missing/audio','/missing/voice'])
        self.assertEqual(self.call()[0],str(self.cache/'model-audio'))

    def test_parallel_install_never_exposes_unverified_pair(self):
        # Use one already-written script so workers do not race while creating
        # their test harness. Real uploads share only the extraction directory.
        self.call()
        for p in [self.cache/'model-audio',self.cache/'model-voice']:p.unlink()
        script=self.root/'run.sh'
        def run(_):
            r=subprocess.run(['sh',str(script)],capture_output=True,text=True,timeout=5)
            self.assertEqual(r.returncode,0,r.stderr)
            pair=r.stdout.splitlines()[:2]
            self.assertIn(pair, [['/missing/audio','/missing/voice'],[str(self.cache/'model-audio'),str(self.cache/'model-voice')]])
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:list(pool.map(run, range(16)))
        for name,source in [('model-audio',self.audio),('model-voice',self.voice)]:
            self.assertEqual(hashlib.sha256((self.cache/name).read_bytes()).digest(),hashlib.sha256(source.read_bytes()).digest())
        self.assertFalse(list(self.cache.glob('.install.*')))


if __name__=='__main__':unittest.main()
