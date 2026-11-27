"""Bounded native-TTS adapter tests. Synthesizer is a fake ubus; never speaks."""
import fcntl,hashlib,io,json,os,shutil,signal,subprocess,tempfile,time,unittest,wave
from pathlib import Path
ROOT=Path(__file__).resolve().parent

class AudioTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name);cls.tool=cls.p/'audio'
        cls.fixture=cls.p/'fixture.mp3';cls.asset=cls.p/'tts_fixture.mp3';cls.bus=cls.p/'ubus';cls.cache=cls.p/'cache';cls.owner=cls.p/'tts-owner'
        cls.bus_source='#!/usr/bin/env python3\nimport json,os,shutil,sys,time\nfrom pathlib import Path\np=Path('+repr(str(cls.p))+')\nr=json.loads(sys.argv[-1]);(p/"request.json").write_text(json.dumps(r))\nassert r["play"]==0 and r["save"]==1\nif (p/"slow-tts").exists():time.sleep(2)\nshutil.copyfile(p/"fixture.mp3",p/"tts_fixture.mp3")\nprint(json.dumps({"code":0,"info":json.dumps({"path":str(p/"tts_fixture.mp3")})}))\n'
        cls.bus.write_text(cls.bus_source)
        cls.bus.chmod(0o755)
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'model-audio.c'),'-DMODEL_UBUS="'+str(cls.bus)+'"','-DMODEL_TTS_PREFIX="'+str(cls.p/'tts_')+'"','-DMODEL_AUDIO_CACHE="'+str(cls.cache)+'"','-DMODEL_TTS_OWNER="'+str(cls.owner)+'"','-DMODEL_TTS_SLOW_FAILURE_MS=500','-DMODEL_CACHE_UID=getuid()','-lcrypto','-o',str(cls.tool)],check=True)
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=440:duration=1:sample_rate=16000','-ac','1','-codec:a','libmp3lame','-write_xing','0',str(cls.fixture)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        if self.cache.is_symlink():self.cache.unlink()
        elif self.cache.exists():shutil.rmtree(self.cache)
        if self.owner.is_symlink():self.owner.unlink()
        elif self.owner.exists():shutil.rmtree(self.owner)
        self.bus.write_text(self.bus_source)
        for name in ['attempts','request.json','slow-tts','inside','overlap']:(self.p/name).unlink(missing_ok=True)
    def call(self,*args,data=b''):
        return subprocess.run([str(self.tool),*map(str,args)],input=data,capture_output=True,timeout=4)
    def name(self,text,greeting=False):
        key=hashlib.sha256(text).hexdigest()
        return ('g-' if greeting else '')+key+'.wav'
    def test_decode_wave_size_matches_data(self):
        r=self.call('--decode',self.fixture);self.assertEqual(r.returncode,0)
        with wave.open(io.BytesIO(r.stdout)) as w:
            self.assertEqual(w.getframerate(),16000);self.assertEqual(w.getnchannels(),1)
            self.assertEqual(len(w.readframes(w.getnframes())),w.getnframes()*2)
            self.assertGreater(w.getnframes(),16000)
    def test_native_synthesis_no_global_play_and_exact_cleanup(self):
        peer=self.p/'tts_peer.mp3';peer.write_bytes(b'leave peer alone')
        r=self.call('--synthesize',data='引号"换行\n反斜线\\'.encode());self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(json.loads((self.p/'request.json').read_text())['text'],'引号"换行\n反斜线\\')
        self.assertFalse(self.asset.exists());self.assertEqual(peer.read_bytes(),b'leave peer alone')
        self.assertTrue(r.stdout.startswith(b'RIFF'))
    def test_reject_symlink_invalid_and_size_bounds_before_output(self):
        symlink=self.p/'link';symlink.symlink_to(self.fixture);self.assertNotEqual(self.call('--decode',symlink).returncode,0)
        for b in [b'not audio',b'x'*(512*1024+1)]:
            f=self.p/'bad';f.write_bytes(b);r=self.call('--decode',f);self.assertNotEqual(r.returncode,0);self.assertEqual(r.stdout,b'')
        for b in [b'',b'a'*541,b'a\0b']:
            self.assertNotEqual(self.call('--synthesize',data=b).returncode,0)
    def test_duration_limit_and_cancellation_under_backpressure(self):
        long=self.p/'long.mp3'
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=duration=31:sample_rate=16000','-codec:a','libmp3lame',str(long)],check=True)
        r=self.call('--decode',long);self.assertNotEqual(r.returncode,0);self.assertEqual(r.stdout,b'')
        proc=subprocess.Popen([str(self.tool),'--decode',str(self.fixture)],stdout=subprocess.PIPE)
        try:
            time.sleep(.1);proc.send_signal(signal.SIGTERM);proc.wait(timeout=1)
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            proc.stdout.close()
    def test_explicit_busy_retries_but_malformed_or_other_error_does_not(self):
        original=self.bus.read_text()
        try:
            for scenario,expected in [('busy',0),('malformed',3),('denied',3)]:
                count=self.p/'attempts';count.unlink(missing_ok=True)
                prefix='''#!/usr/bin/env python3
import json,sys
from pathlib import Path
p=Path(%r);c=p/'attempts';n=int(c.read_text()) if c.exists() else 0;c.write_text(str(n+1))
scenario=%r
if scenario=='malformed':print('{}');sys.exit(0)
if scenario=='denied':print('{"code":403}');sys.exit(0)
if n<2:print('{"code":-1}');sys.exit(0)
'''%(str(self.p),scenario)
                self.bus.write_text(prefix+original.split('\n',1)[1])
                r=self.call('--synthesize',data=('测试'+scenario).encode())
                self.assertEqual(r.returncode,expected,r.stderr)
                self.assertEqual(int(count.read_text()),3 if scenario=='busy' else 1)
        finally:self.bus.write_text(original)

    def test_slow_stock_minus_one_is_not_blindly_resubmitted(self):
        self.bus.write_text('''#!/usr/bin/env python3
import time
from pathlib import Path
p=Path(%r);c=p/'attempts';c.write_text(str(int(c.read_text())+1 if c.exists() else 1))
time.sleep(.65)
print('{"code":-1}')
'''%str(self.p))
        r=self.call('--synthesize',data=b'private utterance')
        self.assertEqual(r.returncode,3,r.stderr);self.assertEqual((self.p/'attempts').read_text(),'1')
        self.assertIn(b'stock receive timeout, not resubmitted',r.stderr)
        self.assertIn(b'code=-1 elapsed_ms=',r.stderr);self.assertNotIn(b'private utterance',r.stderr)
        self.bus.write_text(self.bus_source)
        self.assertEqual(self.call('--synthesize',data=b'next request').returncode,0)

    def _wait_request(self):
        deadline=time.monotonic()+1
        while not (self.p/'request.json').exists() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue((self.p/'request.json').exists(),'fake ubus did not start')

    def test_uncached_requests_share_one_native_synthesis_lease(self):
        self.bus.write_text('''#!/usr/bin/env python3
import json,os,shutil,sys,time
from pathlib import Path
p=Path(%r);(p/'request.json').write_text('{}')
try:f=os.open(p/'inside',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600);os.close(f)
except FileExistsError:(p/'overlap').write_text('overlap');sys.exit(1)
time.sleep(.3)
asset=p/('tts_'+str(os.getpid())+'.mp3');shutil.copyfile(p/'fixture.mp3',asset)
(p/'inside').unlink()
print(json.dumps({'code':0,'info':json.dumps({'path':str(asset)})}))
'''%str(self.p))
        # More than 240 bytes bypasses every short-phrase cache writer lock.
        first=subprocess.Popen([str(self.tool),'--synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        first.stdin.write(b'a'*300);first.stdin.close();first.stdin=None
        self._wait_request()
        second=subprocess.Popen([str(self.tool),'--synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        second_out,second_err=second.communicate(b'b'*300,timeout=3)
        first_out,first_err=first.communicate(timeout=3)
        self.assertEqual(first.returncode,0,first_err);self.assertEqual(second.returncode,0,second_err)
        self.assertTrue(first_out.startswith(b'RIFF') and second_out.startswith(b'RIFF'))
        self.assertFalse((self.p/'overlap').exists());self.assertIn(b'synthesis lease wait_ms=',second_err)

    def test_foreground_rechecks_cache_after_warmer_and_background_never_waits(self):
        (self.p/'slow-tts').write_text('slow')
        text=b'prepared greeting'
        warmer=subprocess.Popen([str(self.tool),'--warm-cache'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        warmer.stdin.write(text);warmer.stdin.close();warmer.stdin=None;self._wait_request()
        began=time.monotonic();busy=self.call('--warm-cache',data=b'other greeting')
        self.assertEqual(busy.returncode,75,busy.stderr);self.assertLess(time.monotonic()-began,.4)
        waiter=subprocess.Popen([str(self.tool),'--synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        waiter_out,waiter_err=waiter.communicate(text,timeout=4)
        warmer_out,warmer_err=warmer.communicate(timeout=1)
        self.assertEqual(warmer.returncode,0,warmer_err);self.assertEqual(warmer_out,b'')
        self.assertEqual(waiter.returncode,0,waiter_err);self.assertTrue(waiter_out.startswith(b'RIFF'))
        self.assertIn(b'cache hit after lease',waiter_err)
        self.assertEqual(json.loads((self.p/'request.json').read_text())['text'],text.decode())

    def test_cached_audio_and_cancelled_waiter_do_not_touch_lease_owner(self):
        self.assertEqual(self.call('--warm-cache',data=b'cached').returncode,0)
        (self.p/'request.json').unlink()
        fd=os.open(self.owner,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        try:
            began=time.monotonic();hit=self.call('--synthesize',data=b'cached')
            self.assertEqual(hit.returncode,0,hit.stderr);self.assertLess(time.monotonic()-began,.4)
            waiter=subprocess.Popen([str(self.tool),'--synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            waiter.stdin.write(b'uncached');waiter.stdin.close();waiter.stdin=None
            time.sleep(.1);began=time.monotonic();waiter.send_signal(signal.SIGTERM)
            out,err=waiter.communicate(timeout=1)
            self.assertEqual(waiter.returncode,130,err);self.assertLess(time.monotonic()-began,.3)
            self.assertEqual(out,b'');self.assertFalse((self.p/'request.json').exists())
        finally:os.close(fd)

    def test_untrusted_synthesis_owner_path_never_submits_native_request(self):
        peer=self.p/'lease-peer';peer.mkdir(exist_ok=True)
        self.owner.symlink_to(peer,target_is_directory=True)
        r=self.call('--synthesize',data=b'untrusted lease')
        self.assertEqual(r.returncode,3,r.stderr);self.assertFalse((self.p/'request.json').exists())
        self.owner.unlink();self.owner.mkdir(mode=0o755)
        self.assertEqual(self.call('--synthesize',data=b'untrusted lease').returncode,3)
        self.assertFalse((self.p/'request.json').exists())

    def test_warm_cache_emits_nothing_then_hit_skips_native_request(self):
        text='我在呢，请说。'.encode()
        r=self.call('--warm-cache',data=text);self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,b'')
        files=list(self.cache.glob('*.wav'));self.assertEqual(len(files),1)
        self.assertEqual(files[0].stat().st_mode&0o777,0o600);self.assertEqual(self.cache.stat().st_mode&0o777,0o700)
        self.assertEqual(files[0].name,self.name(text,True))
        (self.p/'request.json').unlink()
        r=self.call('--synthesize',data=text);self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn(b'cache hit',r.stderr);self.assertFalse((self.p/'request.json').exists())
        with wave.open(io.BytesIO(r.stdout)) as w:self.assertGreater(w.getnframes(),16000)
        self.assertEqual(self.call('--synthesize',data='换个问候'.encode()).returncode,0)
        self.assertTrue((self.p/'request.json').exists())

    def test_prepared_greeting_notifies_actual_pcm_once_in_stream_mode(self):
        text=b'prepared greeting'
        self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        (self.p/'request.json').unlink();rd,wr=os.pipe()
        try:
            proc=subprocess.Popen([str(self.tool),'--stream-synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,pass_fds=(wr,),env={**os.environ,'MODEL_AUDIO_READY_FD':str(wr)})
            os.close(wr);wr=-1;out,err=proc.communicate(text,timeout=2)
            self.assertEqual(proc.returncode,0,err);self.assertTrue(out.startswith(b'RIFF'));self.assertGreater(len(out),44)
            self.assertEqual(os.read(rd,4),b'\x01');self.assertEqual(os.read(rd,1),b'')
            self.assertFalse((self.p/'request.json').exists())
        finally:
            os.close(rd)
            if wr>=0:os.close(wr)

    def test_untrusted_directory_and_symlink_entry_are_never_followed(self):
        peer=self.p/'private-peer';peer.write_bytes(b'leave peer alone')
        self.cache.symlink_to(self.p,target_is_directory=True)
        r=self.call('--synthesize',data=b'hello');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(peer.read_bytes(),b'leave peer alone')
        self.cache.unlink();self.cache.mkdir(mode=0o755)
        self.assertEqual(self.call('--synthesize',data=b'hello').returncode,0)
        self.assertEqual(list(self.cache.iterdir()),[])
        self.cache.chmod(0o700)
        name=self.name(b'hello');(self.cache/name).symlink_to(peer)
        r=self.call('--synthesize',data=b'hello');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(peer.read_bytes(),b'leave peer alone');self.assertTrue((self.cache/name).is_symlink())

    def test_cache_count_space_limit_and_crash_partial_cleanup(self):
        self.cache.mkdir(mode=0o700);partial=self.cache/'.part-999999';partial.write_bytes(b'incomplete');partial.chmod(0o600)
        for i in range(12):self.assertEqual(self.call('--synthesize',data=('短句%d'%i).encode()).returncode,0)
        files=list(self.cache.iterdir());self.assertLessEqual(len(files),8)
        self.assertLessEqual(sum(p.stat().st_size for p in files),256*1024)
        self.assertTrue(all(p.suffix=='.wav' and p.stat().st_mode&0o777==0o600 for p in files))
        self.assertFalse(partial.exists())
        before={p.name for p in files}
        self.assertEqual(self.call('--synthesize',data=b'x'*97).returncode,0)
        self.assertEqual({p.name for p in self.cache.iterdir()},before)

    def test_large_decoded_short_text_falls_back_without_cache_growth(self):
        original=self.fixture.read_bytes()
        try:
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=duration=4:sample_rate=16000','-codec:a','libmp3lame','-y',str(self.fixture)],check=True)
            r=self.call('--synthesize',data=b'short text');self.assertEqual(r.returncode,0,r.stderr)
            self.assertGreater(len(r.stdout),96*1024);self.assertEqual(list(self.cache.iterdir()),[])
            r=self.call('--warm-cache',data=b'short text');self.assertEqual(r.returncode,3,r.stderr)
            self.assertEqual(r.stdout,b'');self.assertEqual(list(self.cache.iterdir()),[])
        finally:self.fixture.write_bytes(original)

    def test_locked_cache_falls_back_without_wait_or_native_warm(self):
        import fcntl
        self.cache.mkdir(mode=0o700)
        fd=os.open(self.cache,os.O_RDONLY|os.O_DIRECTORY)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            r=self.call('--warm-cache',data=b'hello');self.assertEqual(r.returncode,75);self.assertEqual(r.stdout,b'')
            self.assertFalse((self.p/'request.json').exists())
            r=self.call('--synthesize',data=b'hello');self.assertEqual(r.returncode,0,r.stderr)
            self.assertTrue((self.p/'request.json').exists());self.assertEqual(list(self.cache.iterdir()),[])
        finally:os.close(fd)

    def test_status_is_local_silent_and_matches_the_configured_greeting(self):
        text='已经准备好，请说。'.encode()
        r=self.call('--cache-status',data=text);self.assertEqual(r.returncode,4,r.stderr)
        status=json.loads(r.stdout);self.assertEqual(status['status'],'missing')
        self.assertEqual(len(status['cache_key']),64)
        self.assertFalse((self.p/'request.json').exists())
        self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        (self.p/'request.json').unlink()
        r=self.call('--cache-status',data=text);self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(json.loads(r.stdout)['status'],'ready')
        self.assertFalse((self.p/'request.json').exists())
        self.assertEqual(self.call('--cache-status',data=b'changed').returncode,4)
        self.assertEqual(self.call('--cache-status',data=b'x'*241).returncode,3)

    def test_ordinary_answer_cache_is_not_reported_as_a_prepared_greeting(self):
        text=b'hello';self.assertEqual(self.call('--synthesize',data=text).returncode,0)
        self.assertTrue((self.cache/self.name(text)).exists())
        self.assertEqual(self.call('--cache-status',data=text).returncode,4)
        self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        self.assertTrue((self.cache/self.name(text,True)).exists())
        self.assertEqual(self.call('--cache-status',data=text).returncode,0)

    def test_longer_greeting_is_prepared_and_read_without_caching_long_answers(self):
        text=b'x'*120
        self.assertEqual(self.call('--synthesize',data=text).returncode,0)
        self.assertEqual(list(self.cache.iterdir()),[])
        self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        (self.p/'request.json').unlink()
        r=self.call('--synthesize',data=text);self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn(b'cache hit',r.stderr);self.assertFalse((self.p/'request.json').exists())

    def test_tone_change_keeps_prepared_greeting_until_explicit_regeneration(self):
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        old=self.call('--synthesize',data=text).stdout;original=self.fixture.read_bytes()
        try:
            # The fake official synthesizer now produces another tone. Merely
            # changing its audio source must not invalidate the saved greeting.
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=880:duration=1:sample_rate=16000','-ac','1','-codec:a','libmp3lame','-write_xing','0','-y',str(self.fixture)],check=True)
            (self.p/'request.json').unlink()
            self.assertEqual(self.call('--cache-status',data=text).returncode,0)
            self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
            self.assertEqual(self.call('--synthesize',data=text).stdout,old)
            self.assertFalse((self.p/'request.json').exists())
            r=self.call('--regenerate-cache',data=text);self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(r.stdout,b'');self.assertTrue((self.p/'request.json').exists())
            self.assertNotEqual(self.call('--synthesize',data=text).stdout,old)
        finally:self.fixture.write_bytes(original)

    def test_force_regeneration_is_silent_replaces_only_after_success(self):
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        target=self.cache/self.name(text,True);before=target.stat().st_ino
        (self.p/'request.json').unlink()
        r=self.call('--regenerate-cache',data=text)
        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,b'')
        self.assertTrue((self.p/'request.json').exists());self.assertNotEqual(target.stat().st_ino,before)
        self.assertEqual(self.call('--cache-status',data=text).returncode,0)

    def test_failed_force_preserves_old_key_even_under_eviction_pressure(self):
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        target=self.cache/self.name(text,True);old=target.read_bytes();inode=target.stat().st_ino
        for i in range(12):self.assertEqual(self.call('--synthesize',data=('answer%d'%i).encode()).returncode,0)
        self.bus.write_text('#!/usr/bin/env python3\nprint(\'{"code":403}\')\n')
        r=self.call('--regenerate-cache',data=text);self.assertEqual(r.returncode,3,r.stderr)
        self.assertEqual(r.stdout,b'');self.assertEqual(target.read_bytes(),old);self.assertEqual(target.stat().st_ino,inode)
        r=self.call('--synthesize',data=text);self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(r.stdout,old);self.assertIn(b'cache hit',r.stderr)

    def test_prepared_greeting_survives_many_ordinary_answers_with_same_limits(self):
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        target=self.cache/self.name(text,True);old=target.read_bytes()
        for i in range(16):self.assertEqual(self.call('--synthesize',data=('answer%d'%i).encode()).returncode,0)
        self.assertEqual(target.read_bytes(),old);self.assertEqual(self.call('--cache-status',data=text).returncode,0)
        files=list(self.cache.iterdir());self.assertLessEqual(len(files),8)
        self.assertLessEqual(sum(p.stat().st_size for p in files),256*1024)

    def test_full_greeting_cache_skips_ordinary_writes_and_rotates_for_new_greetings(self):
        for i in range(10):self.assertEqual(self.call('--warm-cache',data=('greeting%d'%i).encode()).returncode,0)
        before={p.name:p.read_bytes() for p in self.cache.iterdir()}
        self.assertTrue(before);self.assertTrue(all(n.startswith('g-') for n in before))
        r=self.call('--synthesize',data=b'ordinary answer');self.assertEqual(r.returncode,0,r.stderr)
        self.assertTrue(r.stdout.startswith(b'RIFF'))
        self.assertEqual({p.name:p.read_bytes() for p in self.cache.iterdir()},before)
        self.assertEqual(self.call('--warm-cache',data=b'a new greeting').returncode,0)
        self.assertEqual(self.call('--cache-status',data=b'a new greeting').returncode,0)
        files=list(self.cache.iterdir());self.assertLessEqual(len(files),8)
        self.assertLessEqual(sum(p.stat().st_size for p in files),256*1024)

    def test_force_lock_does_not_block_reading_the_old_prepared_greeting(self):
        import fcntl
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        (self.p/'request.json').unlink();fd=os.open(self.cache,os.O_RDONLY|os.O_DIRECTORY)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.assertEqual(self.call('--cache-status',data=text).returncode,0)
            self.assertEqual(self.call('--regenerate-cache',data=text).returncode,75)
            self.assertEqual(self.call('--synthesize',data=text).returncode,0)
            self.assertFalse((self.p/'request.json').exists())
        finally:os.close(fd)

    def test_cancel_force_keeps_old_greeting_and_removes_its_partial(self):
        text=b'hello';self.assertEqual(self.call('--warm-cache',data=text).returncode,0)
        target=self.cache/self.name(text,True);old=target.read_bytes()
        (self.p/'slow-tts').touch();(self.p/'request.json').unlink()
        proc=subprocess.Popen([str(self.tool),'--regenerate-cache'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        proc.stdin.write(text);proc.stdin.close()
        try:
            end=time.monotonic()+2
            while not (self.p/'request.json').exists() and time.monotonic()<end:time.sleep(.01)
            self.assertTrue((self.p/'request.json').exists())
            proc.terminate();proc.wait(timeout=2)
            self.assertEqual(proc.returncode,130)
        finally:
            if proc.poll() is None:proc.kill();proc.wait()
            proc.stdout.close();proc.stderr.close()
        self.assertEqual(target.read_bytes(),old)
        self.assertEqual(list(self.cache.glob('.part-*')),[])

if __name__=='__main__':unittest.main()
