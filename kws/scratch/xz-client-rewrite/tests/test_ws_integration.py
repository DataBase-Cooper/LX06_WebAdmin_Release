import base64
import hashlib
import os
import socket
import struct
import subprocess
import threading


def recv_exact(sock, count):
    chunks = bytearray()
    while len(chunks) < count:
        part = sock.recv(count - len(chunks))
        if not part:
            raise RuntimeError("peer closed early")
        chunks.extend(part)
    return bytes(chunks)


def serve_once(listener, errors):
    try:
        conn, _ = listener.accept()
        with conn:
            request = bytearray()
            while not request.endswith(b"\r\n\r\n"):
                request.extend(recv_exact(conn, 1))
            headers = request.decode("ascii")
            assert "Authorization: Bearer test-token" in headers
            assert "Protocol-Version: 1" in headers
            assert "Device-Id: 02:00:00:00:00:01" in headers
            assert "Client-Id: 00000000-0000-4000-8000-000000000001" in headers
            key = next(line.split(":", 1)[1].strip() for line in headers.split("\r\n")
                       if line.lower().startswith("sec-websocket-key:"))
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest()).decode("ascii")
            conn.sendall(("HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode("ascii"))

            head = recv_exact(conn, 2)
            assert head[0] == 0x81 and head[1] & 0x80
            length = head[1] & 0x7f
            if length == 126:
                length = struct.unpack(">H", recv_exact(conn, 2))[0]
            elif length == 127:
                length = struct.unpack(">Q", recv_exact(conn, 8))[0]
            mask = recv_exact(conn, 4)
            payload = recv_exact(conn, length)
            message = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
            assert message == b"hello-client", message
            response = b"hello-server"
            conn.sendall(bytes([0x81, len(response)]) + response)
    except Exception as exc:
        errors.append(exc)


def main():
    binary = os.environ.get("XZ_WS_TEST_BINARY", "tests/test_ws_client")
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5)
    port = listener.getsockname()[1]
    errors = []
    thread = threading.Thread(target=serve_once, args=(listener, errors), daemon=True)
    thread.start()
    result = subprocess.run([binary, f"ws://127.0.0.1:{port}/ws"], capture_output=True, text=True)
    thread.join(timeout=6)
    listener.close()
    if errors:
        raise errors[0]
    if result.returncode != 0:
        raise RuntimeError(f"client exited {result.returncode}: {result.stdout}{result.stderr}")
    print(result.stdout.strip())


if __name__ == "__main__":
    main()
