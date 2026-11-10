#!/usr/bin/env python3
"""Real Unix/WebSocket clients: auth, Origin, framing, bounds, serialization and shutdown.

Set BLUEPIXEL_DESKTOP_BINARY to exercise the same transports over the macOS app's displayed CPC.
Desktop settings are isolated under a temporary BLUEPIXEL_DATA_DIR.
"""
import base64
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DESKTOP = os.environ.get("BLUEPIXEL_DESKTOP_BINARY")
BINARY = Path(DESKTOP or os.environ.get("BLUEPIXEL_HEADLESS_BINARY") or ROOT / "build/bluepixel-headless")
if not DESKTOP and os.environ.get("BLUEPIXEL_NO_BUILD") != "1":
    subprocess.run([str(ROOT / "tools/build-headless.sh"), str(BINARY)], check=True, cwd=ROOT)


def exact(sock, count):
    result = bytearray()
    while len(result) < count:
        data = sock.recv(count - len(result))
        assert data, "unexpected EOF"
        result.extend(data)
    return bytes(result)


def line(sock):
    result = bytearray()
    while not result.endswith(b"\n"):
        result.extend(exact(sock, 1))
    return bytes(result)


def request(method, params=None, ident=1):
    return json.dumps({"jsonrpc": "2.0", "id": ident, "method": method, "params": params or {}}, ensure_ascii=False).encode()


def frame(payload, opcode=1, final=True, masked=True):
    payload = bytes(payload)
    n = len(payload)
    header = bytes([(128 if final else 0) | opcode, (128 if masked else 0) | (n if n < 126 else 126 if n <= 65535 else 127)])
    if n >= 126:
        header += struct.pack("!H" if n <= 65535 else "!Q", n)
    if not masked:
        return header + payload
    mask = b"\x11\x22\x33\x44"
    return header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(payload))


def receive(sock):
    head = exact(sock, 2)
    assert head[0] & 128 and not head[1] & 128
    n = head[1] & 127
    if n == 126:
        n = struct.unpack("!H", exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack("!Q", exact(sock, 8))[0]
    assert n < 8 * 1024 * 1024
    return head[0] & 15, exact(sock, n)


class Running:
    def __init__(self, directory):
        self.directory = directory
        self.path = directory / "control.sock"
        self.secret = directory / "token"
        env = dict(os.environ)
        env.setdefault("BLUEPIXEL_ROM_DIR", str(ROOT / "web/roms"))
        if DESKTOP:
            data = directory / "app-data"
            (data / "ROM").mkdir(parents=True, exist_ok=True)
            for name in ("OS6128.ROM", "BASIC1-1.ROM", "AMSDOS.ROM"):
                shutil.copyfile(ROOT / "web/roms" / name, data / "ROM" / name)
            env["BLUEPIXEL_DATA_DIR"] = str(data)
            options = ["--control-server", "--control-unix", str(self.path), "--control-websocket", "0", "--control-token-file", str(self.secret), "--control-origin", "http://localhost:8080"]
        else:
            options = ["--unix", str(self.path), "--websocket", "0", "--token-file", str(self.secret), "--origin", "http://localhost:8080"]
        self.stderr = (directory / "stderr.log").open("w+")
        self.process = subprocess.Popen([str(BINARY), *options], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, env=env)
        ready, _, _ = select.select([self.process.stdout], [], [], 30)
        assert ready, "server did not start"
        raw = self.process.stdout.readline()
        if not raw:
            self.stderr.seek(0)
            raise AssertionError(self.stderr.read())
        status = json.loads(raw)
        assert status["server_ready"]
        self.port = status["websocket_port"]
        self.token = self.secret.read_text().strip()
        assert stat.S_IMODE(self.secret.stat().st_mode) == 0o600
        assert stat.S_IMODE(self.path.stat().st_mode) == 0o600

    def unix(self, token=None):
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(5)
        client.connect(str(self.path))
        client.sendall(("AUTH " + (self.token if token is None else token) + "\n").encode())
        return client

    def ws(self, token=None, origin="http://localhost:8080", extra="", protocol=False, host=None):
        client = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        token = self.token if token is None else token
        headers = f"GET /rpc HTTP/1.1\r\nHost: {host or f'127.0.0.1:{self.port}'}\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n"
        if origin is not None:
            headers += f"Origin: {origin}\r\n"
        if protocol:
            headers += f"Sec-WebSocket-Protocol: bluepixel.v1, bluepixel.token.{token}\r\n"
        else:
            headers += f"Authorization: Bearer {token}\r\n"
        client.sendall((headers + extra + "\r\n").encode())
        response = bytearray()
        while not response.endswith(b"\r\n\r\n"):
            response.extend(exact(client, 1))
        if response.startswith(b"HTTP/1.1 101"):
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            assert b"Sec-WebSocket-Accept: " + accept in response
            assert self.token.encode() not in response
        return client, bytes(response)

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            assert self.process.wait(timeout=10) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
            self.stderr.close()
        assert not self.path.exists(), "owned socket must be removed on graceful shutdown"


def call(client, method, params=None, ident=1):
    client.sendall(request(method, params, ident) + b"\n")
    reply = json.loads(line(client))
    assert reply["id"] == ident and "error" not in reply, reply
    return reply["result"]


def closed_with(server, data, code):
    client, response = server.ws()
    assert response.startswith(b"HTTP/1.1 101")
    with client:
        client.sendall(data)
        opcode, payload = receive(client)
        assert opcode == 8 and struct.unpack("!H", payload[:2])[0] == code, (opcode, payload)


with tempfile.TemporaryDirectory(prefix="bp-server-", dir="/tmp") as tmp:
    directory = Path(tmp)
    if DESKTOP:
        data = directory / "default-app"
        (data / "ROM").mkdir(parents=True)
        for name in ("OS6128.ROM", "BASIC1-1.ROM", "AMSDOS.ROM"):
            shutil.copyfile(ROOT / "web/roms" / name, data / "ROM" / name)
        default = subprocess.run([str(BINARY), "--control-unix", str(directory / "disabled.sock"),
                                  "--control-token-file", str(directory / "disabled.token"),
                                  "--shot", str(directory / "default.png"), "--after", "0.2"],
                                 env=dict(os.environ, BLUEPIXEL_DATA_DIR=str(data)), capture_output=True, timeout=30)
        assert default.returncode == 0 and b"server_ready" not in default.stdout
        assert not (directory / "disabled.sock").exists() and not (directory / "disabled.token").exists()
    else:
        plain = subprocess.run([str(BINARY)], input=request("session.capabilities") + b"\n", capture_output=True,
                               env=dict(os.environ, BLUEPIXEL_ROM_DIR=str(ROOT / "web/roms")), timeout=10)
        assert plain.returncode == 0 and json.loads(plain.stdout)["result"]["api_version"] == "1.3"
    server = Running(directory)
    idle = socket.create_connection(("127.0.0.1", server.port), timeout=5)
    idle.sendall(b"GET ")
    try:
        bad = server.unix("0" * 64)
        assert line(bad) == b"ERR unauthorized\n"
        bad.close()
        client = server.unix()
        assert line(client) == b"OK\n"
        capabilities = call(client, "session.capabilities")
        assert capabilities["machine"] == "CPC 6128" and capabilities["api_version"] == "1.3"
        assert capabilities["transports"] == ["unix", "websocket"]
        call(client, "mem.write", {"address": 0x8000, "value": 42})
        assert call(client, "mem.read", {"address": 0x8000})["hex"] == "2a"
        try:
            client.sendall(b"x" * (1024 * 1024 + 1) + b"\n")
            assert client.recv(1) == b""
        except (ConnectionResetError, BrokenPipeError):
            pass
        client.close()
        for token, origin, extra, host, status in [
            ("0" * 64, "http://localhost:8080", "", None, 401),
            (None, "http://evil.example", "", None, 403),
            (None, "null", "", None, 403),
            (None, "http://localhost:8080.evil.example", "", None, 403),
            (None, "http://localhost:8080", "Origin: http://evil.example\r\n", None, 400),
            (None, "http://localhost:8080", "", "evil.example", 400),
        ]:
            sock, response = server.ws(token, origin, extra, host=host)
            sock.close()
            assert response.startswith(f"HTTP/1.1 {status}".encode()), response
        for protocol, origin in [(False, None), (True, "http://localhost:8080")]:
            ws, response = server.ws(protocol=protocol, origin=origin)
            with ws:
                assert response.startswith(b"HTTP/1.1 101")
                if protocol:
                    assert b"Sec-WebSocket-Protocol: bluepixel.v1\r\n" in response
                message = request("mem.read", {"address": 0x8000})
                ws.sendall(frame(message[:12], final=False) + frame(b"hello", opcode=9) + frame(message[12:], opcode=0))
                assert receive(ws) == (10, b"hello")
                op, body = receive(ws)
                assert op == 1 and json.loads(body)["result"]["hex"] == "2a"
                ws.sendall(frame(request("debug.symbols.set", {"name": "niño", "value": 123})))
                assert json.loads(receive(ws)[1])["result"] is True
                ws.sendall(frame(request("debug.symbols.set", {"name": "x" * 200, "value": 1})))
                assert "error" in json.loads(receive(ws)[1]), "16-bit frame lengths must reach the API"
                image = base64.b64encode((ROOT / "Tests/fixtures/nave/nave.dsk").read_bytes()).decode()
                ws.sendall(frame(request("disc.load", {"drive": 1, "name": "nave.dsk", "data_base64": image})))
                assert json.loads(receive(ws)[1])["result"] is True, "64-bit frame lengths must preserve media bytes"
                ws.sendall(frame(request("screen.render")))
                picture = json.loads(receive(ws)[1])["result"]
                assert len(base64.b64decode(picture["data_base64"])) == 768 * 540 * 4
                ws.sendall(frame(struct.pack("!H", 1000), opcode=8))
                assert receive(ws) == (8, struct.pack("!H", 1000))
        closed_with(server, frame(b"{}", masked=False), 1002)
        closed_with(server, frame(b"\xff"), 1007)
        closed_with(server, frame(b"x", opcode=2), 1003)
        closed_with(server, frame(b"x", opcode=9, final=False), 1002)
        closed_with(server, b"\xc1" + frame(b"{}")[1:], 1002)
        closed_with(server, frame(b"x", opcode=0), 1002)
        closed_with(server, b"\x81\xff" + struct.pack("!Q", 1024 * 1024 + 1), 1009)
        closed_with(server, b"\x81\xfe\x00\x01", 1002)
        def exercise_client(index):
            sock = server.unix()
            with sock:
                assert line(sock) == b"OK\n"
                for value in range(5):
                    call(sock, "mem.write", {"address": 0x9000 + index, "value": value})
                    assert call(sock, "mem.read", {"address": 0x9000 + index})["hex"] == f"{value:02x}"
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(exercise_client, range(4)))
        # Exercise media ownership too: desktop API replacements must not leave stale GUI disc pointers.
        sock = server.unix()
        with sock:
            assert line(sock) == b"OK\n"
            image = base64.b64encode((ROOT / "Tests/fixtures/nave/nave.dsk").read_bytes()).decode()
            for _ in range(2):
                call(sock, "disc.load", {"drive": 0, "name": "nave.dsk", "data_base64": image})
            assert call(sock, "disc.info", {"drive": 0})["name"] == "nave.dsk"
            state = call(sock, "state.save")
            call(sock, "run.frames", {"count": 2})
            call(sock, "state.restore", state)
    finally:
        idle.close()
        server.stop()
    token = server.token
    server = Running(directory)
    assert server.token == token, "safe owner-only token files should be reusable"
    busy = server.unix()
    assert line(busy) == b"OK\n"
    busy.sendall(request("run.frames", {"count": 1000000}) + b"\n")
    time.sleep(0.05)  # ensure shutdown is exercised while an authorized request is running
    started = time.monotonic()
    server.stop()
    assert time.monotonic() - started < 3, "shutdown must cancel long virtual runs"
    busy.close()
    if not DESKTOP:
        for mode in ("symlink", "public", "occupied"):
            location = directory / (mode + ".sock")
            secret = directory / (mode + ".token")
            if mode == "symlink":
                secret.symlink_to(directory / "token")
            elif mode == "public":
                secret.write_text(token); secret.chmod(0o644)
            else:
                location.write_text("preserve me")
                secret = directory / "token"
            result = subprocess.run([str(BINARY), "--unix", str(location), "--token-file", str(secret)],
                                    env=dict(os.environ, BLUEPIXEL_ROM_DIR=str(ROOT / "web/roms")), capture_output=True, timeout=10)
            assert result.returncode != 0
            if mode == "occupied":
                assert location.read_text() == "preserve me"
        # Cleanup only removes the inode created by this server, never a replacement file.
        server = Running(directory)
        server.path.unlink()
        server.path.write_text("replacement")
        server.process.send_signal(signal.SIGTERM)
        assert server.process.wait(timeout=10) == 0
        server.stderr.close()
        assert server.path.read_text() == "replacement"
print("control server: PASS (" + ("desktop" if DESKTOP else "headless") + "; Unix, WebSocket, auth, Origin, framing, limits, concurrent clients, media, restart/shutdown)")
