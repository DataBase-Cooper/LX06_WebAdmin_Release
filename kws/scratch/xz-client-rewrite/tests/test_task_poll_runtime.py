import json
import math
import os
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from websockets.sync.server import serve
from test_runtime_integration import OpusCodec


ROOT = Path(__file__).resolve().parents[1]


def run():
    frame = struct.pack('<960h', *(round(3500 * math.sin(2 * math.pi * 320 * i / 16000))
                                  for i in range(960)))
    silence = bytes(len(frame))
    errors = []
    stop = threading.Event()
    voiced = threading.Event()

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        fifo = path / 'mic'
        os.mkfifo(fifo)
        task_root = path / 'tasks'
        first_session = None
        connections = 0

        def task(status, ident='order_132', query=''):
            state = task_root / os.readlink(task_root / 'active') / (ident+'.state')
            staged = path / 'stage'
            version = 'v2' if query else 'v1'
            staged.write_text(f'{version} {ident} {status}\n' + (query+'\n' if query else ''), encoding='utf-8')
            staged.chmod(0o600)
            staged.replace(state)

        def microphone():
            fd = os.open(fifo, os.O_RDWR)
            try:
                while not stop.wait(.060):
                    os.write(fd, frame if voiced.is_set() else silence)
            finally:
                os.close(fd)

        def handler(ws):
            nonlocal first_session, connections
            def receive():
                while True:
                    message = ws.recv(timeout=6)
                    if isinstance(message, str):
                        return json.loads(message)

            def reply():
                for phase in ('start', 'stop'):
                    ws.send(json.dumps({'type': 'tts', 'state': phase}))

            def foreground():
                voiced.set()
                time.sleep(.42)
                voiced.clear()

            try:
                assert receive()['type'] == 'hello'
                ws.send(json.dumps({'type': 'hello', 'version': 1, 'transport': 'websocket',
                                    'session_id': 'task-runtime',
                                    'audio_params': {'format': 'opus', 'sample_rate': 24000,
                                                     'channels': 1, 'frame_duration': 60}}))
                assert receive()['state'] == 'start'
                current = task_root / os.readlink(task_root / 'active')
                connections += 1
                if connections == 2:
                    assert current != first_session and not first_session.exists()
                    assert not list(current.glob('*.state')), 'previous tasks leaked into a new session'
                    deadline = time.monotonic() + 3.5
                    while time.monotonic() < deadline:
                        assert isinstance(ws.recv(timeout=.5), bytes), 'previous task was queried again'
                    ws.send(json.dumps({'type': 'goodbye'}))
                    return
                first_session = current
                task('running', 'report_847', '报告 {id} 什么时候完成')
                task('ready', query='查询订单 {id}')
                foreground()
                assert receive()['state'] == 'stop', 'a poll displaced an existing user turn'
                reply()
                assert receive()['state'] == 'start'

                query = receive()
                assert query['state'] == 'detect' and query['text'] == '查询订单 order_132'
                foreground()
                assert receive()['type'] == 'abort'
                assert receive()['state'] == 'start'
                captured = []
                while True:
                    data = ws.recv(timeout=4)
                    if isinstance(data, str):
                        assert json.loads(data)['state'] == 'stop'
                        break
                    captured.append(data)
                decoder = OpusCodec()
                assert max(decoder.decode_rms(data, 16000, 960) for data in captured) > 600
                task('queried')
                reply()
                assert receive()['state'] == 'start'
                next_turn = time.monotonic() + 3.5
                while time.monotonic() < next_turn:
                    assert isinstance(ws.recv(timeout=.5), bytes)
                task('ready', 'report_847', '报告 {id} 什么时候完成')
                query = receive()
                assert query['state'] == 'detect' and query['text'] == '报告 report_847 什么时候完成'
                reply()
                assert receive()['state'] == 'start'
                ws.send(json.dumps({'type': 'goodbye'}))
            except BaseException as exc:
                errors.append(exc)

        with serve(handler, '127.0.0.1', 0) as server:
            peer = threading.Thread(target=server.serve_forever, daemon=True)
            writer = threading.Thread(target=microphone, daemon=True)
            peer.start()
            writer.start()
            env = dict(os.environ, XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',
                       XZ_DEVICE_ID='02:00:00:00:00:01', XZ_CLIENT_ID='task-runtime',
                       XZ_ACCESS_TOKEN='fixture', XZ_MIC_FIFO=str(fifo), XZ_WAKE_TEXT='',
                       XZ_BARGE_ANY='', XZ_INTERRUPT_TEXT='', XZ_PLAYBACK='0',
                       XZ_LISTEN_PAUSE_MS='1000', XZ_LISTEN_END_WORD='', XZ_TALK_LOG='0',
                       XZ_TIMELINE_FILE='-', XZ_MCP_TASK_AUTO='1', XZ_MCP_TASK_QUERY='',
                       XZ_MCP_TASK_ROOT=str(task_root))
            result = subprocess.run([str(ROOT / 'build/xz_client')], env=env,
                                    capture_output=True, text=True, timeout=24)
            reopened = subprocess.run([str(ROOT / 'build/xz_client')], env=env,
                                      capture_output=True, text=True, timeout=12)
            stop.set()
            server.shutdown()
            peer.join(2)
            writer.join(2)
    assert not errors, (errors, result.stderr[-5000:], reopened.stderr[-5000:])
    assert connections == 2 and reopened.returncode == 0, reopened.stderr
    assert 'event=MCP_TASK_QUERY ' not in reopened.stderr
    assert result.returncode == 0, result.stderr
    assert result.stderr.count('event=MCP_TASK_QUERY ') == 2
    assert 'event=MCP_TASK_YIELD_TO_SPEECH ' in result.stderr
    print('PASS two task-specific queries with no global template, speech priority, and no query after re-wake')


if __name__ == '__main__':
    run()
