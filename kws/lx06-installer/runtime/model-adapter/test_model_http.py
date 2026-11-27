"""Local TLS/SSE/keepalive/cancellation tests. Never access user accounts."""
import http.server
import os
from pathlib import Path
import re
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parent


class TestServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self):
        super().__init__(('127.0.0.1',0), Handler)
        self.connections = 0
        self.requests = []
        self.release = threading.Event()
        self.warm_started = threading.Event()
        self.handshake_started = threading.Event()
        self.handshake_release = threading.Event()
        self.hold_handshake = False

    def get_request(self):
        item = super().get_request()
        self.handshake_started.set()
        if self.hold_handshake:self.handshake_release.wait(10)
        item[0].do_handshake()
        self.connections += 1
        return item

    def handle_error(self,request,address):
        # Broken TLS peers are intentional cancellation/no-replay fixtures.
        pass


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self,*args):
        pass

    def handle_expect_100(self):
        # A server allowed to accept a body without sending 100 Continue.
        return True

    def do_HEAD(self):
        self.server.requests.append((self.path,b'',dict(self.headers)))
        self.server.warm_started.set()
        if self.path == '/warm-drop':
            self.close_connection=True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        if self.path == '/warm-wait':
            self.server.release.wait(10)
        self.send_response(403 if self.path == '/warm-forbidden' else 200)
        self.send_header('Content-Length','0')
        self.end_headers()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length','0')))
        self.server.requests.append((self.path,body,dict(self.headers)))
        if self.path == '/drop':
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            return
        status = 403 if self.path == '/forbidden' else 200
        data = b'private-provider-error' if status==403 else b'data: first\n\ndata: second\n\n'
        if self.path == '/large':
            data = b'x'*(1048576+8192)
        self.send_response(status)
        self.send_header('Content-Length',str(len(data)))
        self.send_header('Content-Type','text/event-stream')
        self.end_headers()
        try:
            if self.path == '/slow':
                self.wfile.write(data[:13]);self.wfile.flush()
                self.server.release.wait(10)
                self.wfile.write(data[13:]);self.wfile.flush()
            elif self.path == '/stream':
                self.wfile.write(data[:13]);self.wfile.flush()
                time.sleep(.6)
                self.wfile.write(data[13:]);self.wfile.flush()
            else:
                self.wfile.write(data);self.wfile.flush()
        except (BrokenPipeError,ConnectionResetError,ssl.SSLError):
            self.close_connection=True


@unittest.skipUnless(os.geteuid()==0,'production IPC requires root')
class ModelHttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.build.name)/'model-http'
        subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'model-http.c'),'-ldl','-o',str(cls.binary)],check=True)
        cls.cert = Path(cls.build.name)/'cert.pem'
        cls.key = Path(cls.build.name)/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
                        '-keyout',str(cls.key),'-out',str(cls.cert),'-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name);self.root.chmod(0o700)
        self.sock = self.root/'http.sock'
        self.server = TestServer()
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.cert,self.key)
        self.server.socket=context.wrap_socket(self.server.socket,server_side=True,do_handshake_on_connect=False)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.addCleanup(self.stop_server)
        self.start_broker()
        self.addCleanup(self.stop_broker)
        self.wait_socket()

    def start_broker(self,warm=None):
        args=[str(self.binary),'--serve',str(self.sock),'--ca-file',str(self.cert)]
        if warm is not None:args.extend(['--warm',str(warm)])
        self.broker=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)

    def restart_warm(self,warm):
        self.stop_broker();self.start_broker(warm)
        for _ in range(100):
            if (warm/'ready').exists():return
            if self.broker.poll() is not None:self.fail(self.broker.stderr.read().decode())
            time.sleep(.01)
        self.fail('warm listener not ready')

    def stop_server(self):
        self.server.handshake_release.set();self.server.release.set()
        self.server.shutdown();self.server.server_close();self.thread.join(2)

    def stop_broker(self):
        if self.broker.poll() is None:self.broker.terminate()
        self.broker.communicate(timeout=3)

    def wait_socket(self):
        for _ in range(100):
            if self.sock.exists():return
            if self.broker.poll() is not None:self.fail(self.broker.stderr.read().decode())
            time.sleep(.01)
        self.fail('socket not ready')

    def request(self,path='/ok',body=b'{"question":"hello"}',headers=None,method=b'POST',timeout=b'10'):
        directory=self.root/('r'+str(time.monotonic_ns()));directory.mkdir(mode=0o700)
        fields={'method':method,'url':('https://localhost:'+str(self.server.server_port)+path).encode(),
                'headers':headers if headers is not None else b'header = "Content-Type: application/json"\nheader = "Authorization: Bearer synthetic-test"\n',
                'body':body,'timeout':timeout}
        for name,data in fields.items():
            p=directory/name;p.write_bytes(data);p.chmod(0o600)
        return directory

    def client(self,directory,**kwargs):
        return subprocess.run([str(self.binary),'--request',str(self.sock),str(directory)],capture_output=True,timeout=5,**kwargs)

    def start_client(self,directory):
        return subprocess.Popen([str(self.binary),'--request',str(self.sock),str(directory)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)

    def test_tls_keepalive_and_first_sse_bytes_before_completion(self):
        first=self.request('/stream');start=time.monotonic();client=self.start_client(first)
        self.assertEqual(client.stdout.read(13),b'data: first\n\n')
        first_elapsed=time.monotonic()-start
        self.assertIsNone(client.poll())
        rest,error=client.communicate(timeout=3)
        self.assertEqual(client.returncode,0,error)
        self.assertEqual(rest,b'data: second\n\n')
        self.assertLess(first_elapsed,.5)
        second=self.client(self.request())
        self.assertEqual(second.returncode,0,second.stderr)
        self.assertEqual(self.server.connections,1)
        self.broker.terminate();_,log=self.broker.communicate(timeout=3)
        lines=log.decode().splitlines()
        self.assertEqual(len(lines),2)
        self.assertIn('new_connections=1',lines[0]);self.assertIn('new_connections=0',lines[1])
        self.assertNotIn('synthetic-test',log.decode())
        self.assertNotIn('localhost',log.decode())

    def test_cancel_silent_transfer_unblocks_next_request_without_replay(self):
        client=self.start_client(self.request('/slow'))
        self.assertEqual(client.stdout.read(13),b'data: first\n\n')
        start=time.monotonic();client.terminate();client.communicate(timeout=2)
        next_result=self.client(self.request('/ok'))
        self.assertEqual(next_result.returncode,0,next_result.stderr)
        self.assertLess(time.monotonic()-start,3)
        self.assertEqual([r[0] for r in self.server.requests],['/slow','/ok'])

    def test_consumed_post_on_reused_connection_is_never_replayed(self):
        self.assertEqual(self.client(self.request()).returncode,0)
        result=self.client(self.request('/drop'))
        self.assertEqual(result.returncode,1)
        self.assertEqual([r[0] for r in self.server.requests],['/ok','/drop'])
        self.assertEqual(self.client(self.request()).returncode,0)
        self.assertEqual([r[0] for r in self.server.requests],['/ok','/drop','/ok'])

    def test_http_failure_body_is_suppressed_and_status_preserved(self):
        directory=self.request('/forbidden');result=self.client(directory)
        self.assertEqual(result.returncode,22)
        self.assertEqual(result.stdout,b'');self.assertEqual(result.stderr,b'')
        self.assertEqual((directory/'status').read_text(),'403\n')

    def test_large_body_no_implicit_expect_wait_and_response_bound(self):
        start=time.monotonic();result=self.client(self.request(body=b'x'*8192))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertLess(time.monotonic()-start,.7)
        self.assertIsNone(self.server.requests[-1][2].get('Expect'))
        result=self.client(self.request('/large'))
        self.assertEqual(result.returncode,1)
        self.assertLessEqual(len(result.stdout),1048576)

    def test_local_rejection_and_pre_send_unavailable_are_distinct(self):
        directory=self.request();(directory/'body').chmod(0o644)
        self.assertEqual(self.client(directory).returncode,2)
        directory=self.request(body=b'');self.assertEqual(self.client(directory).returncode,2)
        directory=self.request(headers=b'insecure\n');self.assertEqual(self.client(directory).returncode,1)
        self.assertEqual(self.server.requests,[])
        self.broker.terminate();self.broker.communicate(timeout=3)
        directory=self.request();result=self.client(directory)
        self.assertEqual(result.returncode,75)
        self.assertEqual((directory/'status').read_text(),'000\n')

    def test_server_stop_cancels_active_transfer_and_removes_socket(self):
        client=self.start_client(self.request('/slow'))
        self.assertEqual(client.stdout.read(13),b'data: first\n\n')
        start=time.monotonic();self.broker.terminate();self.broker.communicate(timeout=3)
        client.communicate(timeout=2)
        self.assertLess(time.monotonic()-start,3)
        self.assertEqual(client.returncode,1)
        self.assertFalse(self.sock.exists())

    def test_warm_head_queues_first_post_and_reuses_same_tls_connection(self):
        warm=self.request('/warm-wait',body=b'',method=b'HEAD')
        self.restart_warm(warm)
        self.assertEqual((warm/'ready').stat().st_mode&0o7777,0o600)
        self.assertTrue(self.server.warm_started.wait(2))
        client=self.start_client(self.request())
        time.sleep(.15)
        self.assertIsNone(client.poll())
        self.assertEqual([r[0] for r in self.server.requests],['/warm-wait'])
        self.server.release.set();out,error=client.communicate(timeout=3)
        self.assertEqual(client.returncode,0,error)
        self.assertEqual(out,b'data: first\n\ndata: second\n\n')
        self.assertEqual(self.server.connections,1)
        self.broker.terminate();_,log=self.broker.communicate(timeout=3)
        lines=log.decode().splitlines()
        self.assertIn('warm=1 new_connections=1',lines[0])
        self.assertIn('new_connections=0',lines[1])
        self.assertFalse((warm/'ready').exists())
        self.assertNotIn('synthetic-test',log.decode())
        self.assertNotIn('localhost',log.decode())

    def test_anonymous_head_http_error_still_retains_transport(self):
        warm=self.request('/warm-forbidden',body=b'',method=b'HEAD')
        self.restart_warm(warm)
        self.assertEqual(self.client(self.request()).returncode,0)
        self.assertEqual(self.server.connections,1)
        self.broker.terminate();_,log=self.broker.communicate(timeout=3)
        self.assertRegex(log.decode(),r'warm=1 .*http=403 curl_code=0')
        self.assertNotIn('warm_transport_failed',log.decode())

    def test_listener_ready_before_slow_tls_handshake_and_first_post_waits(self):
        self.server.hold_handshake=True
        warm=self.request(body=b'',method=b'HEAD');self.restart_warm(warm)
        self.assertTrue(self.server.handshake_started.wait(2))
        client=self.start_client(self.request())
        time.sleep(.15)
        self.assertIsNone(client.poll())
        self.assertEqual(self.server.requests,[])
        self.server.handshake_release.set();_,error=client.communicate(timeout=3)
        self.assertEqual(client.returncode,0,error)
        self.assertEqual(self.server.connections,1)

    def test_client_canceled_while_queued_does_not_submit_post_after_warm(self):
        warm=self.request('/warm-wait',body=b'',method=b'HEAD');self.restart_warm(warm)
        self.assertTrue(self.server.warm_started.wait(2))
        client=self.start_client(self.request('/canceled'))
        time.sleep(.1);client.terminate();client.communicate(timeout=2)
        self.server.release.set()
        self.assertEqual(self.client(self.request()).returncode,0)
        self.assertEqual([r[0] for r in self.server.requests],['/warm-wait','/ok'])

    def test_failed_warm_uses_fresh_pool_without_post_replay(self):
        warm=self.request('/warm-drop',body=b'',method=b'HEAD')
        self.restart_warm(warm)
        self.assertEqual(self.client(self.request()).returncode,0)
        self.assertEqual(self.client(self.request('/drop')).returncode,1)
        self.assertEqual(self.client(self.request()).returncode,0)
        self.assertEqual([r[0] for r in self.server.requests],['/warm-drop','/ok','/drop','/ok'])
        self.assertGreaterEqual(self.server.connections,3)
        self.broker.terminate();_,log=self.broker.communicate(timeout=3)
        self.assertIn('warm_transport_failed=1',log.decode())

    def test_warm_timeout_is_bounded_and_following_post_remains_usable(self):
        warm=self.request('/warm-wait',body=b'',method=b'HEAD',timeout=b'1')
        self.restart_warm(warm)
        self.assertTrue(self.server.warm_started.wait(2))
        start=time.monotonic();result=self.client(self.request())
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertLess(time.monotonic()-start,2)
        self.assertGreaterEqual(self.server.connections,2)

    def test_stop_during_warm_cancels_head_and_cleans_ready(self):
        warm=self.request('/warm-wait',body=b'',method=b'HEAD')
        self.restart_warm(warm)
        self.assertTrue(self.server.warm_started.wait(2))
        start=time.monotonic();self.stop_broker()
        self.assertLess(time.monotonic()-start,3)
        self.assertFalse(self.sock.exists())
        self.assertFalse((warm/'ready').exists())

    def test_warm_refuses_post_and_unsafe_private_files_without_network(self):
        cases=[]
        cases.append(self.request())
        cases.append(self.request(body=b'',method=b'GET'))
        unsafe=self.request(body=b'',method=b'HEAD');(unsafe/'headers').chmod(0o644);cases.append(unsafe)
        linked=self.request(body=b'',method=b'HEAD');(linked/'body').unlink();(linked/'body').symlink_to(self.cert);cases.append(linked)
        self.stop_broker()
        for warm in cases:
            self.start_broker(warm);_,log=self.broker.communicate(timeout=3)
            self.assertEqual(self.broker.returncode,2,log)
            self.assertFalse(self.sock.exists())
            self.assertFalse((warm/'ready').exists())
        self.assertEqual(self.server.requests,[])

    def test_ready_marker_does_not_replace_or_remove_existing_file(self):
        warm=self.request(body=b'',method=b'HEAD')
        marker=warm/'ready';marker.write_text('script-owned');marker.chmod(0o600)
        self.stop_broker();self.start_broker(warm);_,log=self.broker.communicate(timeout=3)
        self.assertEqual(self.broker.returncode,2,log)
        self.assertEqual(marker.read_text(),'script-owned')
        self.assertFalse(self.sock.exists())
        self.assertEqual(self.server.requests,[])

    def test_shutdown_preserves_replacement_ready_marker(self):
        warm=self.request(body=b'',method=b'HEAD')
        self.restart_warm(warm)
        self.assertEqual(self.client(self.request()).returncode,0)
        # Keep the old inode alive to make replacement deterministic.
        (warm/'ready').rename(warm/'old-ready')
        marker=warm/'ready';marker.write_text('replacement');marker.chmod(0o600)
        self.stop_broker()
        self.assertEqual(marker.read_text(),'replacement')


if __name__=='__main__':
    unittest.main()
