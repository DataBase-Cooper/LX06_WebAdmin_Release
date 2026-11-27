"""Local smart-home submission and the native reply share one turn."""
import base64
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

from test_runtime_integration import OpusCodec, recv_client_frame, recv_exact, send_server_binary, send_server_text


def write_executable(path, source):
    path.write_text(source, encoding="utf-8")
    path.chmod(0o700)


def run(binary, voice_amplitude):
    with tempfile.TemporaryDirectory(prefix="lx06-home-feedback-") as temporary:
        root = Path(temporary)
        (root / "mic.raw").write_bytes(bytes(1920 * 4))
        write_executable(root / "mijia_mcp.sh", """#!/bin/sh
# LX06-HOME-MCP: 1
test "$1" = --home-mcp-action && test "$2" = off || exit 2
printf '%s\\n' "$3" >> calls
case "$3" in
    书桌灯|床头灯) printf '{"code":0}\\n';;
    坏灯) printf '{"code":1}\\n';;
    *) exit 2;;
esac
""")
        write_executable(root / "aplay", """#!/bin/sh
set -eu
n=1
while test -e "play-$n.raw"; do n=$((n + 1)); done
cat > "play-$n.raw"
""")
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)
            port = listener.getsockname()[1]

            def gateway():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(8)
                        request = bytearray()
                        while not request.endswith(b"\r\n\r\n"):
                            request.extend(recv_exact(conn, 1))
                        key = next(line.split(b":", 1)[1].strip() for line in request.split(b"\r\n")
                                   if line.lower().startswith(b"sec-websocket-key:"))
                        accept = base64.b64encode(hashlib.sha1(
                            key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
                        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                     b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                        opcode, message = recv_client_frame(conn)
                        assert opcode == 1 and json.loads(message)["features"]["mcp"]
                        send_server_text(conn, {
                            "type": "hello", "version": 1, "transport": "websocket", "session_id": "test-home",
                            "audio_params": {"format": "opus", "sample_rate": 24000, "channels": 1,
                                             "frame_duration": 60},
                        })

                        def wait_for_listen():
                            while True:
                                opcode, message = recv_client_frame(conn)
                                if opcode == 1:
                                    packet = json.loads(message)
                                    if packet["type"] == "listen" and packet["state"] == "start":
                                        return
                                    assert packet["type"] != "abort" and packet.get("state") != "detect"

                        def reply(text, amplitude=3000):
                            send_server_text(conn, {"type": "tts", "state": "start"})
                            send_server_text(conn, {"type": "tts", "state": "sentence_start", "text": text})
                            send_server_binary(conn, codec.encode_constant(24000, 1440, amplitude))
                            send_server_text(conn, {"type": "tts", "state": "stop"})

                        def tool_result(device, accepted):
                            send_server_text(conn, {"type": "mcp", "session_id": "test-home", "payload": {
                                "jsonrpc": "2.0", "id": 42, "method": "tools/call", "params": {
                                    "name": "self.smart_home.control",
                                    "arguments": {"action": "off", "device": device},
                                },
                            }})
                            while True:
                                opcode, message = recv_client_frame(conn)
                                if opcode == 1:
                                    packet = json.loads(message)
                                    assert packet["type"] == "mcp", packet
                                    result = packet["payload"]["result"]
                                    assert result["isError"] is not accepted
                                    assert ("操作成功" in result["content"][0]["text"]) is accepted
                                    break

                        codec = OpusCodec()
                        wait_for_listen()
                        send_server_text(conn, {"type": "stt", "text": "关闭书桌的灯。"})
                        end = time.monotonic() + 1
                        while time.monotonic() < end:
                            if (root / "calls").exists():
                                break
                            time.sleep(.01)
                        assert (root / "calls").read_text().splitlines() == ["书桌灯"]
                        tool_result("书桌灯", True)
                        reply("已经帮你关掉书桌灯啦。", voice_amplitude)
                        wait_for_listen()
                        time.sleep(.3)
                        send_server_text(conn, {"type": "stt", "text": "把床头灯关了。"})
                        tool_result("床头灯", True)
                        # The gateway can choose different words and cadence
                        # on every turn, even when using the same voice.
                        reply("床头灯关好啦。", -voice_amplitude)
                        wait_for_listen()
                        time.sleep(.3)
                        send_server_text(conn, {"type": "stt", "text": "把坏灯关了。"})
                        tool_result("坏灯", False)
                        reply("这次没有提交成功。")
                        wait_for_listen()
                        send_server_text(conn, {"type": "stt", "text": "今天星期几？"})
                        reply("普通对话依然有声音")
                        wait_for_listen()
                        send_server_text(conn, {"type": "goodbye"})
                except Exception:
                    errors.append(traceback.format_exc())

            thread = threading.Thread(target=gateway, daemon=True)
            thread.start()
            env = {**os.environ, "XZ_WS_URL": f"ws://127.0.0.1:{port}/ws",
                   "XZ_DEVICE_ID": "02:00:00:00:00:01", "XZ_CLIENT_ID": "test-home-client",
                   "XZ_ACCESS_TOKEN": "test-only-token", "XZ_MIC_FIFO": "mic.raw",
                   "XZ_APLAY": str(root / "aplay"), "XZ_TIMELINE_FILE": "-", "XZ_WAKE_TEXT": "",
                   "XZ_TALK_LOG": "talk", "XZ_HOME_MCP": "1", "XZ_HOME_MCP_SCRIPT": "mijia_mcp.sh",
                   "XZ_PLAYBACK_TAIL_MUTE_MS": "15"}
            process = subprocess.run([str(binary)], cwd=root, env=env, capture_output=True, text=True, timeout=20)
            thread.join(3)
        assert not errors and not thread.is_alive(), errors
        assert process.returncode == 0, process.stdout + process.stderr
        assert (root / "calls").read_text().splitlines() == ["书桌灯", "床头灯", "坏灯"]
        assert "event=HOME_MCP_VOICE_SUBMITTED" in process.stderr + process.stdout
        assert (root / "play-1.raw").read_bytes() != (root / "play-2.raw").read_bytes()
        for number in (1, 2, 3, 4):
            assert len((root / f"play-{number}.raw").read_bytes()) == 2880
        log = (root / "talk").read_text()
        assert all(text in log for text in ("已经帮你关掉书桌灯啦。", "这次没有提交成功。", "普通对话依然有声音"))
        return (root / "play-1.raw").read_bytes()


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    # Two connections can reuse the server's session ID with different voices.
    assert run(binary, 1000) != run(binary, -2000)
    print("smart home native reply runtime: PASS")
