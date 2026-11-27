import base64
import ctypes
import ctypes.util
import hashlib
import json
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def recv_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise RuntimeError("peer closed before frame completed")
        data.extend(part)
    return bytes(data)


def recv_client_frame(sock):
    head = recv_exact(sock, 2)
    opcode = head[0] & 0x0F
    assert head[0] & 0x80, "fragmented client frames are not expected"
    assert head[1] & 0x80, "client frames must be masked"
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4)
    payload = recv_exact(sock, length)
    return opcode, bytes(value ^ mask[i & 3] for i, value in enumerate(payload))


def send_server_text(sock, value):
    payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
    if len(payload) < 126:
        header = bytes((0x81, len(payload)))
    else:
        assert len(payload) <= 0xFFFF
        header = bytes((0x81, 126)) + struct.pack(">H", len(payload))
    sock.sendall(header + payload)


def send_server_binary(sock, payload):
    assert len(payload) <= 0xFFFF
    if len(payload) < 126:
        header = bytes((0x82, len(payload)))
    else:
        header = bytes((0x82, 126)) + struct.pack(">H", len(payload))
    sock.sendall(header + payload)


class OpusCodec:
    APPLICATION_VOIP = 2048

    def __init__(self):
        library = ctypes.util.find_library("opus")
        assert library, "libopus is required for the duplex suppression test"
        self.lib = ctypes.CDLL(library)
        self.lib.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                                 ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                                 ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16),
                                         ctypes.c_int, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int]
        self.lib.opus_encode.restype = ctypes.c_int
        self.lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte),
                                         ctypes.c_int, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                         ctypes.c_int]
        self.lib.opus_decode.restype = ctypes.c_int

    def encode_constant(self, sample_rate, frame_size, value):
        error = ctypes.c_int()
        encoder = self.lib.opus_encoder_create(sample_rate, 1, self.APPLICATION_VOIP,
                                               ctypes.byref(error))
        assert encoder and error.value == 0
        try:
            pcm = (ctypes.c_int16 * frame_size)(*([value] * frame_size))
            packet = (ctypes.c_ubyte * 4000)()
            length = self.lib.opus_encode(encoder, pcm, frame_size, packet, len(packet))
            assert length > 0
            return bytes(packet[:length])
        finally:
            self.lib.opus_encoder_destroy(encoder)

    def decode_rms(self, packet, sample_rate, frame_size):
        error = ctypes.c_int()
        decoder = self.lib.opus_decoder_create(sample_rate, 1, ctypes.byref(error))
        assert decoder and error.value == 0
        try:
            encoded = (ctypes.c_ubyte * len(packet)).from_buffer_copy(packet)
            pcm = (ctypes.c_int16 * frame_size)()
            decoded = self.lib.opus_decode(decoder, encoded, len(packet), pcm, frame_size, 0)
            assert decoded == frame_size
            return (sum(sample * sample for sample in pcm[:decoded]) / decoded) ** 0.5
        finally:
            self.lib.opus_decoder_destroy(decoder)


def run_protocol_round(binary):
    mic_path = ROOT / "build" / "runtime-test-mic.raw"
    mic_path.parent.mkdir(parents=True, exist_ok=True)
    mic_path.write_bytes(bytes(1920 * 3))

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    errors = []
    server_observed = {"audio_frames": 0, "mcp_reply": False}

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(8)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(recv_exact(conn, 1))
                headers = request.decode("ascii")
                assert "Authorization: Bearer test-only-token" in headers
                assert "Protocol-Version: 1" in headers
                assert "Device-Id: 02:00:00:00:00:01" in headers
                assert "Client-Id: 00000000-0000-4000-8000-000000000001" in headers
                key = next(
                    line.split(":", 1)[1].strip()
                    for line in headers.split("\r\n")
                    if line.lower().startswith("sec-websocket-key:")
                )
                accept = base64.b64encode(
                    hashlib.sha1(
                        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                    ).digest()
                ).decode("ascii")
                conn.sendall(
                    (
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                    ).encode("ascii")
                )

                opcode, payload = recv_client_frame(conn)
                assert opcode == 1
                hello = json.loads(payload)
                assert hello["type"] == "hello"
                assert hello["version"] == 1
                assert hello["transport"] == "websocket"
                assert hello["audio_params"] == {
                    "format": "opus",
                    "sample_rate": 16000,
                    "channels": 1,
                    "frame_duration": 60,
                }
                assert "aec" not in hello.get("features", {})
                assert "mcp" not in hello.get("features", {})

                send_server_text(
                    conn,
                    {
                        "type": "hello",
                        "version": 1,
                        "transport": "websocket",
                        "session_id": "local-test-session",
                        "audio_params": {
                            "format": "opus",
                            "sample_rate": 24000,
                            "channels": 1,
                            "frame_duration": 60,
                        },
                    },
                )

                client_text = []
                deadline = 8
                while len(client_text) < 2 and deadline:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        client_text.append(json.loads(payload))
                    deadline -= 1
                assert [item["state"] for item in client_text] == ["detect", "start"]
                assert client_text[0]["text"] == "小爱同学召唤小智"
                assert client_text[1]["mode"] == "auto"
                assert all(item["session_id"] == "local-test-session" for item in client_text)

                while server_observed["audio_frames"] < 3:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 2:
                        assert payload, "empty Opus packets must not be sent"
                        server_observed["audio_frames"] += 1
                    else:
                        client_text.append(json.loads(payload))

                send_server_text(conn, {"type": "tts", "state": "start"})
                send_server_text(conn, {"type": "tts", "state": "sentence_start", "text": "test"})
                send_server_text(conn, {"type": "tts", "state": "stop"})
                send_server_text(
                    conn,
                    {
                        "type": "mcp",
                        "session_id": "local-test-session",
                        "payload": {"jsonrpc": "2.0", "id": 17, "method": "tools/list"},
                    },
                )
                send_server_text(conn, {"type": "goodbye"})
                while True:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        rearm = json.loads(payload)
                        assert rearm["state"] == "start"
                        assert rearm["mode"] == "auto"
                        continue
                    assert opcode == 8, "client must not answer unsupported MCP"
                    break
        except Exception:  # surfaced in the calling test thread
            import traceback
            errors.append(traceback.format_exc())
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    env = os.environ.copy()
    env.update(
        {
            "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
            "XZ_DEVICE_ID": "02:00:00:00:00:01",
            "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
            "XZ_ACCESS_TOKEN": "test-only-token",
            "XZ_MIC_FIFO": str(mic_path),
            "XZ_WAKE_TEXT": "小爱同学召唤小智",
            "XZ_TIMELINE_FILE": "-",
        }
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=15)
    thread.join(timeout=10)
    mic_path.unlink(missing_ok=True)
    assert not errors, errors
    assert not thread.is_alive(), "mock server did not finish"
    assert result.returncode == 0, result.stdout + result.stderr
    assert server_observed == {"audio_frames": 3, "mcp_reply": False}
    combined = result.stdout + result.stderr
    for event in ("WS_CONNECTED", "HELLO_OK", "LISTEN_START", "MIC_FRAME", "TTS_START", "TTS_STOP", "GOODBYE"):
        assert f"event={event}" in combined, f"missing {event} in timeline"
    assert "test-only-token" not in combined


def run_default_tts_uplink_suppression_round(binary):
    fifo_path = Path("/tmp") / f"xz-runtime-test-mic-{os.getpid()}.fifo"
    fifo_path.unlink(missing_ok=True)
    os.mkfifo(fifo_path)
    codec = OpusCodec()
    write_second_frame = threading.Event()
    writer_ready = threading.Event()
    writer_errors = []
    errors = []
    observed = {}

    speech_frame = struct.pack("<960h", *[
        6000 if (index % 40) < 20 else -6000 for index in range(960)
    ])

    def write_microphone():
        try:
            with fifo_path.open("wb", buffering=0) as stream:
                writer_ready.set()
                stream.write(speech_frame)
                assert write_second_frame.wait(8), "server never began TTS"
                for _ in range(4):
                    stream.write(speech_frame)
        except Exception as exc:
            writer_errors.append(f"{type(exc).__name__}: {exc}")

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(8)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(recv_exact(conn, 1))
                key = next(
                    line.split(":", 1)[1].strip()
                    for line in request.decode("ascii").split("\r\n")
                    if line.lower().startswith("sec-websocket-key:")
                )
                accept = base64.b64encode(
                    hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
                ).decode("ascii")
                conn.sendall(
                    ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii")
                )
                opcode, payload = recv_client_frame(conn)
                assert opcode == 1 and json.loads(payload)["type"] == "hello"
                send_server_text(conn, {
                    "type": "hello", "version": 1, "transport": "websocket",
                    "session_id": "suppression-test-session",
                    "audio_params": {"format": "opus", "sample_rate": 24000,
                                     "channels": 1, "frame_duration": 60},
                })
                expected_states = ["detect", "start"]
                while expected_states:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        assert json.loads(payload)["state"] == expected_states.pop(0)
                while "before_tts" not in observed:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 2:
                        observed["before_tts"] = codec.decode_rms(payload, 16000, 960)
                assert observed["before_tts"] > 1200
                send_server_text(conn, {"type": "tts", "state": "start"})
                send_server_binary(conn, codec.encode_constant(24000, 1440, 1000))
                write_second_frame.set()
                observed["during_tts"] = []
                while len(observed["during_tts"]) < 4:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 2:
                        observed["during_tts"].append(codec.decode_rms(payload, 16000, 960))
                send_server_text(conn, {"type": "goodbye"})
                while True:
                    opcode, _ = recv_client_frame(conn)
                    if opcode == 8:
                        break
        except Exception:
            import traceback
            errors.append(traceback.format_exc())
        finally:
            listener.close()

    server = threading.Thread(target=serve, daemon=True)
    writer = threading.Thread(target=write_microphone, daemon=True)
    server.start()
    env = os.environ.copy()
    env.update({
        "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
        "XZ_DEVICE_ID": "02:00:00:00:00:01",
        "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
        "XZ_MIC_FIFO": str(fifo_path),
        "XZ_PLAYBACK": "0",
        "XZ_TIMELINE_FILE": "-",
    })
    process = subprocess.Popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=env)
    writer.start()
    assert writer_ready.wait(8), "client did not open the microphone FIFO"
    stdout, stderr = process.communicate(timeout=15)
    server.join(timeout=10)
    writer.join(timeout=10)
    fifo_path.unlink(missing_ok=True)
    assert not errors, errors
    assert not writer_errors, writer_errors
    assert not server.is_alive(), "mock server did not finish"
    assert process.returncode == 0, stdout + stderr
    assert observed["during_tts"][-1] < 900, observed


def run_tts_stop_drains_playback_when_server_closes(binary):
    """A final TTS reply must drain even if the server closes immediately.

    On LX06 the PNS microphone producer stops after the first few frames when
    the local output PCM remains open.  The fake player records its stdin EOF,
    which must arrive on the protocol TTS stop event rather than only when the
    whole websocket session ends.
    """
    mic_path = ROOT / "build" / f"runtime-stop-release-{os.getpid()}.raw"
    mic_path.write_bytes(bytes(1920 * 6))
    codec = OpusCodec()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    errors = []
    released = threading.Event()
    released_before_goodbye = {"value": False}

    with tempfile.TemporaryDirectory(prefix="xz-player-release-") as temp:
        temp_path = Path(temp)
        marker = temp_path / "released"
        player = temp_path / "fake-aplay"
        player.write_text(
            "#!/bin/sh\ncat >/dev/null\nsleep 0.3\nprintf released > \"$XZ_PLAYER_RELEASE_MARKER\"\n",
            encoding="utf-8",
        )
        player.chmod(0o700)

        def serve():
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(8)
                    request = bytearray()
                    while not request.endswith(b"\r\n\r\n"):
                        request.extend(recv_exact(conn, 1))
                    key = next(
                        line.split(":", 1)[1].strip()
                        for line in request.decode("ascii").split("\r\n")
                        if line.lower().startswith("sec-websocket-key:")
                    )
                    accept = base64.b64encode(
                        hashlib.sha1(
                            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                        ).digest()
                    ).decode("ascii")
                    conn.sendall(
                        ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii")
                    )
                    opcode, payload = recv_client_frame(conn)
                    assert opcode == 1 and json.loads(payload)["type"] == "hello"
                    send_server_text(conn, {
                        "type": "hello", "version": 1, "transport": "websocket",
                        "session_id": "release-test-session",
                        "audio_params": {"format": "opus", "sample_rate": 24000,
                                         "channels": 1, "frame_duration": 60},
                    })
                    states = ["detect", "start"]
                    while states:
                        opcode, payload = recv_client_frame(conn)
                        if opcode == 1:
                            assert json.loads(payload)["state"] == states.pop(0)
                    while True:
                        opcode, _ = recv_client_frame(conn)
                        if opcode == 2:
                            break
                    send_server_text(conn, {"type": "tts", "state": "start"})
                    send_server_binary(conn, codec.encode_constant(24000, 1440, 1000))
                    send_server_text(conn, {"type": "tts", "state": "stop"})
                    conn.sendall(b"\x88\x00")
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline and not marker.exists():
                        time.sleep(0.02)
                    released_before_goodbye["value"] = marker.exists()
                    released.set()
            except Exception:
                import traceback
                errors.append(traceback.format_exc())
            finally:
                listener.close()

        server = threading.Thread(target=serve, daemon=True)
        server.start()
        env = os.environ.copy()
        env.update({
            "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
            "XZ_DEVICE_ID": "02:00:00:00:00:01",
            "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
            "XZ_MIC_FIFO": str(mic_path),
            "XZ_PLAYBACK": "1",
            "XZ_APLAY": str(player),
            "XZ_PLAYER_RELEASE_MARKER": str(marker),
            "XZ_TIMELINE_FILE": "-",
        })
        process = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=15)
        server.join(timeout=10)
        assert not errors, errors
        assert not server.is_alive(), "mock server did not finish"
        assert released.is_set(), "mock server never checked playback release"
        assert released_before_goodbye["value"], (
            "server close aborted the final TTS instead of draining its playback pipe"
        )
        assert process.returncode == 0, process.stdout + process.stderr
        assert "event=PLAYBACK_EXIT exit=0" in process.stdout + process.stderr

    mic_path.unlink(missing_ok=True)


def run_tts_stop_keeps_uplink_muted_until_player_exit(binary):
    """PCM buffered while aplay drains must never become the next user turn."""
    fifo_path = Path("/tmp") / f"xz-runtime-stop-mute-{os.getpid()}.fifo"
    fifo_path.unlink(missing_ok=True)
    os.mkfifo(fifo_path)
    codec = OpusCodec()
    initial_frame = struct.pack("<960h", *([3000] * 960))
    speech_frame = struct.pack("<960h", *([6000] * 960))
    write_after_stop = threading.Event()
    writer_ready = threading.Event()
    writer_errors = []
    errors = []
    observed = {"during_drain": [], "restart_after_tts": False}

    with tempfile.TemporaryDirectory(prefix="xz-player-drain-") as temp:
        temp_path = Path(temp)
        released = temp_path / "released"
        player = temp_path / "slow-aplay"
        player.write_text(
            "#!/bin/sh\ncat >/dev/null\nsleep 0.4\nprintf released > \"$XZ_PLAYER_RELEASE_MARKER\"\n",
            encoding="utf-8",
        )
        player.chmod(0o700)

        def write_microphone():
            try:
                with fifo_path.open("wb", buffering=0) as stream:
                    writer_ready.set()
                    stream.write(initial_frame)
                    assert write_after_stop.wait(8), "server never sent TTS stop"
                    for _ in range(4):
                        stream.write(speech_frame)
                    deadline = time.monotonic() + 4
                    while not released.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    assert released.exists(), "player did not finish its drain"
                    # A single post-release frame can be read by the client
                    # before its re-arm and arrive muted; space several frames
                    # so at least one lands after the re-arm.  The client exits
                    # on the server's goodbye, so a broken pipe here is the
                    # normal end of the test, not an error.
                    for _ in range(4):
                        try:
                            stream.write(speech_frame)
                        except BrokenPipeError:
                            break
                        time.sleep(0.03)
            except Exception as exc:
                writer_errors.append(f"{type(exc).__name__}: {exc}")

        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(8)
        port = listener.getsockname()[1]

        def serve():
            try:
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(8)
                    request = bytearray()
                    while not request.endswith(b"\r\n\r\n"):
                        request.extend(recv_exact(conn, 1))
                    key = next(
                        line.split(":", 1)[1].strip()
                        for line in request.decode("ascii").split("\r\n")
                        if line.lower().startswith("sec-websocket-key:")
                    )
                    accept = base64.b64encode(
                        hashlib.sha1(
                            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                        ).digest()
                    ).decode("ascii")
                    conn.sendall(
                        ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii")
                    )
                    opcode, payload = recv_client_frame(conn)
                    assert opcode == 1 and json.loads(payload)["type"] == "hello"
                    send_server_text(conn, {
                        "type": "hello", "version": 1, "transport": "websocket",
                        "session_id": "stop-mute-test-session",
                        "audio_params": {"format": "opus", "sample_rate": 24000,
                                         "channels": 1, "frame_duration": 60},
                    })
                    states = ["detect", "start"]
                    while states:
                        opcode, payload = recv_client_frame(conn)
                        if opcode == 1:
                            assert json.loads(payload)["state"] == states.pop(0)
                    while True:
                        opcode, _ = recv_client_frame(conn)
                        if opcode == 2:
                            break
                    send_server_text(conn, {"type": "tts", "state": "start"})
                    send_server_binary(conn, codec.encode_constant(24000, 1440, 1000))
                    send_server_text(conn, {"type": "tts", "state": "stop"})
                    write_after_stop.set()
                    while len(observed["during_drain"]) < 4:
                        opcode, payload = recv_client_frame(conn)
                        if opcode == 2:
                            observed["during_drain"].append(
                                codec.decode_rms(payload, 16000, 960)
                            )
                    assert all(rms < 900 for rms in observed["during_drain"]), observed
                    observed["after_release"] = []
                    while len(observed["after_release"]) < 8:
                        opcode, payload = recv_client_frame(conn)
                        if opcode == 1:
                            rearm = json.loads(payload)
                            assert rearm["state"] == "start"
                            assert rearm["mode"] == "auto"
                            assert released.exists(), "server re-arm preceded local player release"
                            observed["restart_after_tts"] = True
                            continue
                        if opcode == 2:
                            observed["after_release"].append(
                                codec.decode_rms(payload, 16000, 960)
                            )
                            if (observed["restart_after_tts"] and
                                    observed["after_release"][-1] > 1200):
                                break
                    assert any(rms > 1200 for rms in observed["after_release"]), observed
                    assert observed["restart_after_tts"], observed
                    send_server_text(conn, {"type": "goodbye"})
                    while True:
                        opcode, _ = recv_client_frame(conn)
                        if opcode == 8:
                            break
            except Exception:
                import traceback
                errors.append(traceback.format_exc())
            finally:
                listener.close()

        server = threading.Thread(target=serve, daemon=True)
        writer = threading.Thread(target=write_microphone, daemon=True)
        server.start()
        env = os.environ.copy()
        env.update({
            "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
            "XZ_DEVICE_ID": "02:00:00:00:00:01",
            "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
            "XZ_MIC_FIFO": str(fifo_path),
            "XZ_PLAYBACK": "1",
            "XZ_APLAY": str(player),
            "XZ_PLAYER_RELEASE_MARKER": str(released),
            "XZ_PLAYBACK_TAIL_MUTE_MS": "0",
            "XZ_TIMELINE_FILE": "-",
        })
        process = subprocess.Popen([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=env)
        writer.start()
        assert writer_ready.wait(8), "client did not open the microphone FIFO"
        stdout, stderr = process.communicate(timeout=15)
        server.join(timeout=10)
        writer.join(timeout=10)
        assert not errors, errors
        assert not writer_errors, writer_errors
        assert not server.is_alive(), "mock server did not finish"
        assert process.returncode == 0, stdout + stderr
        timeline = stdout + stderr
        exit_index = timeline.index("event=PLAYBACK_EXIT")
        resume_index = timeline.index("event=LISTEN_RESUME")
        assert exit_index < resume_index, timeline
        restart_index = timeline.rindex("event=LISTEN_START")
        assert exit_index < restart_index < resume_index, timeline

    fifo_path.unlink(missing_ok=True)


def run_goodbye_phrase_ends_session(binary):
    mic_path = ROOT / "build" / "runtime-test-mic-goodbye.raw"
    mic_path.parent.mkdir(parents=True, exist_ok=True)
    mic_path.write_bytes(bytes(1920 * 3))

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    errors = []
    observed = {"goodbye_sent": False, "rearm_after_reply": False}

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(8)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(recv_exact(conn, 1))
                headers = request.decode("ascii")
                assert "Authorization: Bearer test-only-token" in headers
                key = next(
                    line.split(":", 1)[1].strip()
                    for line in headers.split("\r\n")
                    if line.lower().startswith("sec-websocket-key:")
                )
                accept = base64.b64encode(
                    hashlib.sha1(
                        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                    ).digest()
                ).decode("ascii")
                conn.sendall(
                    (
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                    ).encode("ascii")
                )
                opcode, payload = recv_client_frame(conn)
                assert opcode == 1 and json.loads(payload)["type"] == "hello"
                send_server_text(
                    conn,
                    {
                        "type": "hello",
                        "version": 1,
                        "transport": "websocket",
                        "session_id": "local-test-session",
                        "audio_params": {
                            "format": "opus",
                            "sample_rate": 24000,
                            "channels": 1,
                            "frame_duration": 60,
                        },
                    },
                )
                client_text = []
                deadline = 8
                while len(client_text) < 2 and deadline:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        client_text.append(json.loads(payload))
                    deadline -= 1
                assert [item["state"] for item in client_text] == ["detect", "start"]
                for _ in range(2):
                    opcode, _ = recv_client_frame(conn)
                    assert opcode == 2, "expected microphone Opus frames"

                send_server_text(conn, {"type": "stt", "text": "小智再见"})
                send_server_text(conn, {"type": "tts", "state": "start", "text": "再见"})
                send_server_text(conn, {"type": "tts", "state": "stop"})

                # In-flight microphone Opus frames may still be queued ahead of
                # the goodbye text frame; skip binary frames and take the next
                # text frame.
                reply = None
                for _ in range(8):
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        reply = json.loads(payload)
                        break
                assert reply is not None, "client must answer the goodbye phrase with text"
                if reply["type"] == "goodbye":
                    observed["goodbye_sent"] = True
                    assert reply["session_id"] == "local-test-session"
                elif reply["type"] == "listen":
                    observed["rearm_after_reply"] = True
                else:
                    raise AssertionError(f"unexpected client event {reply}")

                send_server_text(conn, {"type": "goodbye"})
                while True:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 8:
                        break
                    if opcode == 1:
                        raise AssertionError(f"unexpected client text {payload!r}")
        except Exception:
            import traceback
            errors.append(traceback.format_exc())
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    env = os.environ.copy()
    env.update(
        {
            "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
            "XZ_DEVICE_ID": "02:00:00:00:00:01",
            "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
            "XZ_ACCESS_TOKEN": "test-only-token",
            "XZ_MIC_FIFO": str(mic_path),
            "XZ_WAKE_TEXT": "小爱同学召唤小智",
            "XZ_GOODBYE_TEXT": "小智再见",
            "XZ_TIMELINE_FILE": "-",
        }
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=15)
    thread.join(timeout=10)
    mic_path.unlink(missing_ok=True)
    assert not errors, errors
    assert not thread.is_alive(), "mock server did not finish"
    assert result.returncode == 0, result.stdout + result.stderr
    assert observed == {"goodbye_sent": True, "rearm_after_reply": False}, observed
    combined = result.stdout + result.stderr
    for event in ("STT_RECOGNIZED", "GOODBYE_WORD_DETECTED", "TTS_STOP", "GOODBYE_SENT", "GOODBYE"):
        assert f"event={event}" in combined, f"missing {event} in timeline"
    assert "event=LISTEN_RESUME" not in combined, "goodbye phrase must replace the re-arm"
    assert "test-only-token" not in combined


def run_bare_zaijian_keeps_rearming(binary):
    mic_path = ROOT / "build" / "runtime-test-mic-bare-zaijian.raw"
    mic_path.parent.mkdir(parents=True, exist_ok=True)
    mic_path.write_bytes(bytes(1920 * 3))

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(8)
    port = listener.getsockname()[1]
    errors = []
    observed = {"rearm_after_reply": False, "goodbye_sent": False}

    def serve():
        try:
            conn, _ = listener.accept()
            with conn:
                conn.settimeout(8)
                request = bytearray()
                while not request.endswith(b"\r\n\r\n"):
                    request.extend(recv_exact(conn, 1))
                headers = request.decode("ascii")
                assert "Authorization: Bearer test-only-token" in headers
                key = next(
                    line.split(":", 1)[1].strip()
                    for line in headers.split("\r\n")
                    if line.lower().startswith("sec-websocket-key:")
                )
                accept = base64.b64encode(
                    hashlib.sha1(
                        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                    ).digest()
                ).decode("ascii")
                conn.sendall(
                    (
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                    ).encode("ascii")
                )
                opcode, payload = recv_client_frame(conn)
                assert opcode == 1 and json.loads(payload)["type"] == "hello"
                send_server_text(
                    conn,
                    {
                        "type": "hello",
                        "version": 1,
                        "transport": "websocket",
                        "session_id": "local-test-session",
                        "audio_params": {
                            "format": "opus",
                            "sample_rate": 24000,
                            "channels": 1,
                            "frame_duration": 60,
                        },
                    },
                )
                client_text = []
                deadline = 8
                while len(client_text) < 2 and deadline:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        client_text.append(json.loads(payload))
                    deadline -= 1
                assert [item["state"] for item in client_text] == ["detect", "start"]
                for _ in range(2):
                    opcode, _ = recv_client_frame(conn)
                    assert opcode == 2, "expected microphone Opus frames"

                send_server_text(conn, {"type": "stt", "text": "再见"})
                send_server_text(conn, {"type": "tts", "state": "start"})
                send_server_text(conn, {"type": "tts", "state": "stop"})

                # In-flight microphone Opus frames may still be queued ahead of
                # the re-arm text frame; skip binary frames and take the next
                # text frame.
                reply = None
                for _ in range(8):
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 1:
                        reply = json.loads(payload)
                        break
                assert reply is not None, "client must answer a normal reply with text"
                if reply["type"] == "listen":
                    observed["rearm_after_reply"] = True
                    assert reply["state"] == "start" and reply["mode"] == "auto"
                elif reply["type"] == "goodbye":
                    observed["goodbye_sent"] = True
                else:
                    raise AssertionError(f"unexpected client event {reply}")

                send_server_text(conn, {"type": "goodbye"})
                while True:
                    opcode, payload = recv_client_frame(conn)
                    if opcode == 8:
                        break
                    if opcode == 1:
                        raise AssertionError(f"unexpected client text {payload!r}")
        except Exception:
            import traceback
            errors.append(traceback.format_exc())
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    env = os.environ.copy()
    env.update(
        {
            "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
            "XZ_DEVICE_ID": "02:00:00:00:00:01",
            "XZ_CLIENT_ID": "00000000-0000-4000-8000-000000000001",
            "XZ_ACCESS_TOKEN": "test-only-token",
            "XZ_MIC_FIFO": str(mic_path),
            "XZ_WAKE_TEXT": "小爱同学召唤小智",
            "XZ_GOODBYE_TEXT": "小智再见",
            "XZ_TIMELINE_FILE": "-",
        }
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=15)
    thread.join(timeout=10)
    mic_path.unlink(missing_ok=True)
    assert not errors, errors
    assert not thread.is_alive(), "mock server did not finish"
    assert result.returncode == 0, result.stdout + result.stderr
    assert observed == {"rearm_after_reply": True, "goodbye_sent": False}, observed
    combined = result.stdout + result.stderr
    for event in ("STT_RECOGNIZED", "LISTEN_RESUME", "GOODBYE"):
        assert f"event={event}" in combined, f"missing {event} in timeline"
    assert "event=GOODBYE_WORD_DETECTED" not in combined, "bare 再见 is not the configured phrase"
    assert "event=GOODBYE_SENT" not in combined, "bare 再见 must not send protocol goodbye"
    assert "test-only-token" not in combined


def run_hello_retry(binary):
    errors = []
    with tempfile.TemporaryDirectory(prefix="xz-hello-retry-") as directory, socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        listener.settimeout(8)
        mic = Path(directory) / "mic.raw"
        mic.write_bytes(bytes(1920))

        def serve():
            try:
                for attempt in range(2):
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(8)
                        request = bytearray()
                        while not request.endswith(b"\r\n\r\n"):
                            request.extend(recv_exact(conn, 1))
                        key = next(line.split(b":", 1)[1].strip()
                                   for line in request.splitlines()
                                   if line.lower().startswith(b"sec-websocket-key:"))
                        accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
                        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                     b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                        opcode, payload = recv_client_frame(conn)
                        hello = json.loads(payload)
                        assert opcode == 1 and hello["type"] == "hello"
                        assert "aec" not in hello.get("features", {})
                        if attempt == 0:
                            opcode, _ = recv_client_frame(conn)
                            assert opcode == 8, "no wake phrase or mic audio before server hello"
                        else:
                            send_server_text(conn, {
                                "type": "hello", "version": 1, "transport": "websocket",
                                "session_id": "retry",
                                "audio_params": {"format": "opus", "sample_rate": 24000,
                                                 "channels": 1, "frame_duration": 60},
                            })
                            for state in ("detect", "start"):
                                opcode, payload = recv_client_frame(conn)
                                msg = json.loads(payload)
                                assert opcode == 1 and msg["type"] == "listen" and msg["state"] == state
                            conn.sendall(b"\x88\x00")
                            try:
                                while conn.recv(4096):
                                    pass
                            except ConnectionResetError:
                                pass  # A final microphone frame can race shutdown.
            except Exception as exc:
                errors.append(exc)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        env = {**os.environ, "XZ_WS_URL": f"ws://127.0.0.1:{listener.getsockname()[1]}/",
               "XZ_DEVICE_ID": "02:00:00:00:00:01", "XZ_CLIENT_ID": "retry-fixture",
               "XZ_MIC_FIFO": str(mic), "XZ_PLAYBACK": "1", "XZ_LOCAL_AEC": "1",
               "XZ_INTERRUPT_TEXT": "",
               "XZ_TIMELINE_FILE": "-"}
        env.pop("XZ_BARGE_ANY", None)
        env.pop("XZ_SERVER_AEC", None)
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=12)
        thread.join(timeout=2)
        assert not errors and not thread.is_alive(), errors
        trace = result.stdout + result.stderr
        assert result.returncode == 0, trace
        assert trace.count("event=HELLO_SENT ") == 2, trace
        assert trace.index("event=HELLO_RETRY ") < trace.index("event=HELLO_OK ") < trace.index("event=MIC_OPEN ")


def main():
    binary = Path(os.environ.get("XZ_CLIENT_BINARY", ROOT / "build" / "xz_client"))
    assert binary.exists(), f"build the client first: {binary}"
    run_protocol_round(binary)
    run_hello_retry(binary)
    run_default_tts_uplink_suppression_round(binary)
    run_tts_stop_drains_playback_when_server_closes(binary)
    run_tts_stop_keeps_uplink_muted_until_player_exit(binary)
    run_goodbye_phrase_ends_session(binary)
    run_bare_zaijian_keeps_rearming(binary)
    print("runtime integration tests: PASS")


if __name__ == "__main__":
    main()
