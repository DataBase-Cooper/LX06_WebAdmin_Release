"""Real C incremental MP3/owned-drain tests; fake loopback TTS, no accounts/audio."""
import fcntl,io,json,os,select,shutil,signal,socket,struct,subprocess,tempfile,threading,time,unittest,wave
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

ROOT=Path(__file__).resolve().parent

class StreamingAudioTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name);cls.tool=cls.p/'audio'
        cls.fixture=cls.p/'fixture.mp3';cls.bus=cls.p/'ubus';cls.owner=cls.p/'owner'
        cls.case=None
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                case=cls.case;case.requests.append(self.path)
                self.send_response(case.http_status);self.send_header('Content-Type','audio/mpeg');self.end_headers()
                if case.http_status!=200:return
                payload=cls.fixture.read_bytes();chunks=case.chunks
                try:
                    at=0;part=0
                    while at<len(payload):
                        n=chunks[part%len(chunks)];self.wfile.write(payload[at:at+n]);self.wfile.flush()
                        at+=n;part+=1
                        if part==1:case.first_body.set();time.sleep(case.hold)
                        if case.delay:time.sleep(case.delay)
                    case.server_completed.set()
                except (BrokenPipeError,ConnectionResetError):case.server_disconnected.set()
        cls.server=ThreadingHTTPServer(('127.0.0.1',0),Handler);cls.thread=threading.Thread(target=cls.server.serve_forever,daemon=True);cls.thread.start()
        port=cls.server.server_port
        cls.bus.write_text('''#!/usr/bin/env python3
import json,shutil,sys,time
from pathlib import Path
p=Path(%r);r=json.loads(sys.argv[-1]);(p/'request.json').write_text(json.dumps(r))
if (p/'slow-request').exists():time.sleep(.55)
if r['save']==1:
 asset=p/'tts_saved.mp3';shutil.copyfile(p/'fixture.mp3',asset)
 print(json.dumps({'code':0,'info':json.dumps({'path':str(asset)})}));sys.exit(0)
assert r['caller']=='mediaplayer' and r['save']==0 and r['play']==1
url='http://localhost:%d/'+str(p/'tts_stream.mp3')
if (p/'bad-url').exists():url='http://example.invalid/tts.mp3'
print(json.dumps({'code':0,'info':json.dumps({'path':url})}))
'''%(str(cls.p),port));cls.bus.chmod(0o755)
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'model-audio.c'),
                        '-DMODEL_UBUS="'+str(cls.bus)+'"','-DMODEL_TTS_PREFIX="'+str(cls.p/'tts_')+'"',
                        '-DMODEL_AUDIO_CACHE="'+str(cls.p/'cache')+'"','-DMODEL_TTS_OWNER="'+str(cls.owner)+'"',
                        '-DMODEL_TTS_HTTP_PORT='+str(port),'-DMODEL_TTS_STREAM_MS=1600',
                        '-DMODEL_CACHE_UID=getuid()','-lcrypto','-o',str(cls.tool)],check=True)
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=440:duration=2:sample_rate=16000',
                        '-ac','1','-codec:a','libmp3lame','-write_xing','0',str(cls.fixture)],check=True)
        cls.baseline=subprocess.run([str(cls.tool),'--decode',str(cls.fixture)],capture_output=True,check=True).stdout
    @classmethod
    def tearDownClass(cls):cls.server.shutdown();cls.server.server_close();cls.thread.join();cls.tmp.cleanup()
    def setUp(self):
        type(self).case=self;self.requests=[];self.http_status=200;self.chunks=[2048,73,509];self.hold=.35;self.delay=0
        self.first_body=threading.Event();self.server_completed=threading.Event();self.server_disconnected=threading.Event()
        for name in ('bad-url','slow-request','request.json'):(self.p/name).unlink(missing_ok=True)
        shutil.rmtree(self.p/'cache',ignore_errors=True)
        self.broker=subprocess.Popen([str(self.tool),'--serve-stream'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE,start_new_session=True)
        self.broker_log=b'';deadline=time.monotonic()+1
        while not (self.owner/'stream.sock').exists() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue((self.owner/'stream.sock').exists(),'broker did not start')
    def stop_broker(self):
        if self.broker is not None:
            self.broker.send_signal(signal.SIGTERM)
            try:self.broker_log=self.broker.communicate(timeout=3)[1]
            except subprocess.TimeoutExpired:
                os.killpg(self.broker.pid,signal.SIGKILL);self.broker.communicate();self.fail('broker failed bounded native shutdown')
            self.assertEqual(self.broker.returncode,0,self.broker_log);self.broker=None
        return self.broker_log
    def tearDown(self):self.stop_broker()
    def start(self,extra_fds=(),env=None):
        proc=subprocess.Popen([str(self.tool),'--stream-synthesize'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,start_new_session=True,pass_fds=extra_fds,env=env)
        proc.stdin.write(b'streaming test');proc.stdin.close();proc.stdin=None
        return proc
    def read_audio(self,proc,size=1024,timeout=1.5):
        deadline=time.monotonic()+timeout;out=b''
        while len(out)<size and time.monotonic()<deadline:
            if select.select([proc.stdout],[],[],max(0,deadline-time.monotonic()))[0]:
                data=os.read(proc.stdout.fileno(),size-len(out))
                if not data:break
                out+=data
        return out
    def assert_lease_released(self):
        fd=os.open(self.owner,os.O_RDONLY|os.O_DIRECTORY)
        try:fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        finally:os.close(fd)
    def test_pcm_and_ready_arrive_before_native_eof_and_match_complete_decode(self):
        rd,wr=os.pipe()
        try:
            proc=self.start((wr,),{**os.environ,'MODEL_AUDIO_READY_FD':str(wr)});os.close(wr);wr=-1
            initial=self.read_audio(proc)
            self.assertGreater(len(initial),44);self.assertTrue(initial.startswith(b'RIFF'))
            self.assertIsNone(proc.poll());self.assertFalse(self.server_completed.is_set())
            self.assertEqual(os.read(rd,4),b'\x01')
            rest,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0,err)
            self.assertEqual((initial+rest)[44:],self.baseline[44:])
            self.assertIn(b'stream first PCM',err);self.assertIn(b'discarded=0',self.stop_broker())
            self.assertEqual(len(self.requests),1);self.assertTrue(self.server_completed.is_set());self.assert_lease_released()
        finally:
            os.close(rd)
            if wr>=0:os.close(wr)
    def test_fragmented_input_keeps_every_pcm_sample(self):
        payload=self.fixture.read_bytes();proc=subprocess.Popen([str(self.tool),'--decode-stream'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        # Write on a thread while stdout is drained: deliberate frame and header splits.
        def writer():
            at=0
            while at<len(payload):
                n=(1,3,7,137,509)[at%5];proc.stdin.write(payload[at:at+n]);proc.stdin.flush();at+=n
            proc.stdin.close()
        thread=threading.Thread(target=writer);thread.start()
        out=proc.stdout.read();err=proc.stderr.read();thread.join();proc.wait(timeout=2)
        self.assertEqual(proc.returncode,0,err);self.assertEqual(out[44:],self.baseline[44:])
        proc.stdout.close();proc.stderr.close()
    def test_cancel_mutes_owned_group_while_silent_relay_drains_and_closes_session_fds(self):
        self.hold=.7;keeper_rd,keeper_wr=os.pipe()
        try:
            proc=self.start((keeper_wr,));os.close(keeper_wr);keeper_wr=-1
            initial=self.read_audio(proc);self.assertGreater(len(initial),44)
            began=time.monotonic();os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=.4)
            self.assertLess(time.monotonic()-began,.3);self.assertEqual(proc.returncode,130)
            # The native drainer is still active but must not retain keeper/session descriptors.
            self.assertFalse(self.server_completed.is_set())
            self.assertTrue(select.select([keeper_rd],[],[],.2)[0]);self.assertEqual(os.read(keeper_rd,1),b'')
            self.assertTrue(self.server_completed.wait(2));tail,err=proc.communicate(timeout=2)
            self.assertFalse(self.server_disconnected.is_set());self.assertIn(b'discarded=1',self.stop_broker())
            self.assertLess(len(initial+tail),len(self.baseline));self.assert_lease_released()
        finally:
            os.close(keeper_rd)
            if keeper_wr>=0:os.close(keeper_wr)
    def test_hard_kill_cannot_leave_native_reader_without_drain(self):
        self.hold=.7;proc=self.start();self.assertGreater(len(self.read_audio(proc)),44)
        os.killpg(proc.pid,signal.SIGKILL);proc.wait(timeout=.4)
        self.assertTrue(self.server_completed.wait(2));out,err=proc.communicate(timeout=2)
        self.assertFalse(self.server_disconnected.is_set());self.assertIn(b'discarded=1',self.stop_broker());self.assert_lease_released()
    def test_remote_asset_url_is_never_fetched(self):
        (self.p/'bad-url').write_text('bad')
        proc=self.start();out,err=proc.communicate(timeout=2)
        self.assertEqual(proc.returncode,3,err);self.assertEqual(out,b'');self.assertEqual(self.requests,[]);self.assert_lease_released()
    def test_bad_http_status_never_becomes_pcm(self):
        self.http_status=403;proc=self.start();out,err=proc.communicate(timeout=2)
        self.assertEqual(proc.returncode,3,err);self.assertEqual(out,b'');self.assert_lease_released()
    def test_native_stream_timeout_is_bounded(self):
        self.hold=1.9;proc=self.start();out,err=proc.communicate(timeout=3)
        self.assertEqual(proc.returncode,3,err);self.assertIn(b'status=3',self.stop_broker());self.assert_lease_released()
        time.sleep(.4) # Allow this fake server's bounded sleeping handler to finish before next case.

    def test_cancel_before_url_keeps_posted_request_and_late_fifo_owned_by_broker(self):
        (self.p/'slow-request').write_text('slow');proc=self.start()
        deadline=time.monotonic()+1
        while not (self.p/'request.json').exists() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue((self.p/'request.json').exists());self.assertFalse(self.first_body.is_set())
        began=time.monotonic();os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=.4)
        self.assertEqual(proc.returncode,130);self.assertLess(time.monotonic()-began,.3)
        self.assertTrue(self.server_completed.wait(2));out,err=proc.communicate(timeout=1)
        self.assertEqual(out,b'');self.assertFalse(self.server_disconnected.is_set())
        self.assertIn(b'discarded=1',self.stop_broker());self.assert_lease_released()

    def test_missing_broker_falls_back_before_request_without_duplicate_synthesis(self):
        self.stop_broker();proc=self.start();out,err=proc.communicate(timeout=2)
        self.assertEqual(proc.returncode,0,err);self.assertEqual(out,self.baseline)
        self.assertIn(b'before request; file synthesis fallback',err)
        self.assertEqual(json.loads((self.p/'request.json').read_text())['save'],1);self.assertEqual(self.requests,[])

    def test_broker_rejects_invalid_utf8_nul_and_oversized_requests_before_ubus(self):
        for text in (b'\x00',b'\xc0\xaf',b'\xed\xa0\x80',b'\xf4\x90\x80\x80',b'x'*541):
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
                client.settimeout(1);client.connect(str(self.owner/'stream.sock'))
                client.sendall(b'LXT1'+struct.pack('!I',len(text))+text)
                try:self.assertEqual(client.recv(1),b'')
                except ConnectionResetError:pass
        self.assertFalse((self.p/'request.json').exists());self.assertEqual(self.requests,[])

    def test_readiness_probe_does_not_submit_tts(self):
        result=subprocess.run([str(self.tool),'--stream-ready'],capture_output=True,timeout=2)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout,b'')
        self.assertFalse((self.p/'request.json').exists());self.assertEqual(self.requests,[])
        self.stop_broker()
        self.assertEqual(subprocess.run([str(self.tool),'--stream-ready'],capture_output=True,timeout=2).returncode,75)

    def test_stopping_broker_drains_posted_native_work_before_exit(self):
        self.hold=.6;proc=self.start();initial=self.read_audio(proc);self.assertGreater(len(initial),44)
        self.broker.send_signal(signal.SIGTERM);self.assertIsNone(self.broker.poll())
        rest,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0,err)
        self.assertEqual((initial+rest)[44:],self.baseline[44:]);self.assertTrue(self.server_completed.is_set())
        self.assertFalse(self.server_disconnected.is_set());self.assertIn(b'discarded=0',self.stop_broker())
        self.assertFalse((self.owner/'stream.sock').exists());self.assert_lease_released()

    def test_service_group_term_drains_already_posted_native_work(self):
        self.hold=.6;proc=self.start();initial=self.read_audio(proc);self.assertGreater(len(initial),44)
        os.killpg(self.broker.pid,signal.SIGTERM)
        rest,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0,err)
        self.assertEqual((initial+rest)[44:],self.baseline[44:]);self.assertTrue(self.server_completed.is_set())
        self.assertFalse(self.server_disconnected.is_set());self.assertIn(b'discarded=0',self.stop_broker())
        self.assert_lease_released()

    def test_service_group_term_before_url_keeps_native_ubus_reply_owned(self):
        (self.p/'slow-request').write_text('slow');proc=self.start()
        deadline=time.monotonic()+1
        while not (self.p/'request.json').exists() and time.monotonic()<deadline:time.sleep(.01)
        self.assertTrue((self.p/'request.json').exists());self.assertFalse(self.first_body.is_set())
        os.killpg(self.broker.pid,signal.SIGTERM)
        out,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0,err)
        self.assertEqual(out[44:],self.baseline[44:]);self.assertTrue(self.server_completed.is_set())
        self.assertFalse(self.server_disconnected.is_set());self.stop_broker();self.assert_lease_released()

    def test_service_group_term_cancels_queued_work_before_submission(self):
        lease=os.open(self.owner,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
        try:
            proc=self.start();time.sleep(.12)
            self.assertFalse((self.p/'request.json').exists())
            os.killpg(self.broker.pid,signal.SIGTERM)
            out,err=proc.communicate(timeout=2);self.assertEqual(proc.returncode,3,err);self.assertEqual(out,b'')
            self.stop_broker()
            self.assertFalse((self.p/'request.json').exists());self.assertEqual(self.requests,[])
        finally:os.close(lease)

    def test_cancelled_queued_client_does_not_start_another_native_request(self):
        lease=os.open(self.owner,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
        try:
            proc=self.start();time.sleep(.12)
            self.assertFalse((self.p/'request.json').exists())
            os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=.4)
            self.assertEqual(proc.returncode,130);proc.communicate(timeout=1)
            # Release after the cancelled worker has observed peer closure.
            time.sleep(.08);fcntl.flock(lease,fcntl.LOCK_UN);time.sleep(.08)
            self.assertFalse((self.p/'request.json').exists());self.assertEqual(self.requests,[])
        finally:os.close(lease)

    def test_native_worker_has_broker_parent_and_no_client_process_ancestry(self):
        self.hold=.6;proc=self.start();initial=self.read_audio(proc);self.assertGreater(len(initial),44)
        self.assertEqual(Path('/proc/%d/task/%d/children'%(proc.pid,proc.pid)).read_text().strip(),'')
        children=Path('/proc/%d/task/%d/children'%(self.broker.pid,self.broker.pid)).read_text().split()
        self.assertTrue(children)
        for child in children:
            status=Path('/proc/'+child+'/status').read_text()
            self.assertIn('PPid:\t'+str(self.broker.pid)+'\n',status)
        rest,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0,err)
        self.assertEqual((initial+rest)[44:],self.baseline[44:])

if __name__=='__main__':unittest.main()
