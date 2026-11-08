#!/usr/bin/env python3
"""Run the GlasnostFWK nave input and determinism acceptance using JSON-RPC stdio."""
import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "build" / "bluepixel-headless"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Client:
    def __init__(self):
        self.process = subprocess.Popen(
            [str(BINARY)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        self.ident = 0
        self.frames = 0
        self.method_time = defaultdict(float)

    def call(self, method, params=None):
        self.ident += 1
        request = {"jsonrpc": "2.0", "id": self.ident, "method": method, "params": params or {}}
        started = time.perf_counter()
        self.process.stdin.write(json.dumps(request, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        self.method_time[method] += time.perf_counter() - started
        if not line:
            error = self.process.stderr.readline().strip()
            raise RuntimeError(error or f"headless exited during {method}")
        reply = json.loads(line)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']['message']}")
        return reply["result"]

    def run_frames(self, count):
        self.call("run.frames", {"count": count})
        self.frames += count

    def close(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=5)
        if code:
            detail = self.process.stderr.read().strip()
            raise RuntimeError(detail or f"headless exited with {code}")


def peek(client, expression):
    return int(client.call("mem.read", {"address": expression, "length": 1})["hex"], 16)


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def write_png(path, rgba, width, height):
    stride = width * 4
    rows = b"".join(b"\0" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    content = b"\x89PNG\r\n\x1a\n"
    content += png_chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
    content += png_chunk(b"IDAT", zlib.compress(rows, 6))
    content += png_chunk(b"IEND", b"")
    Path(path).write_bytes(content)


def move_to(client, row, mask, expression, target, timeout):
    client.call("input.matrix", {"row": row, "mask": mask, "pressed": True})
    elapsed = 0
    while elapsed < timeout:
        step = min(5, timeout - elapsed)
        client.run_frames(step)
        elapsed += step
        if peek(client, expression) == target:
            break
    else:
        client.call("input.matrix", {"row": row, "mask": mask, "pressed": False})
        raise RuntimeError(f"timeout waiting for {expression} == {target}")
    client.run_frames(15)
    actual = peek(client, expression)
    client.call("input.matrix", {"row": row, "mask": mask, "pressed": False})
    require(actual == target, f"{expression} moved past boundary: {actual}, expected {target}")


def movement_sequence(client, fixture, image_path=None):
    xexpr, yexpr = "game_storage+4", "game_storage+5"
    drones_before = [peek(client, f"game_storage+37*{i}+4") for i in range(1, 4)]
    client.run_frames(25)
    drones_after = [peek(client, f"game_storage+37*{i}+4") for i in range(1, 4)]
    require(all(a != b for a, b in zip(drones_before, drones_after)), "drone patrol positions did not all change")

    steps = fixture["keyboard_matrix"]
    limits = fixture["limits"]
    right = steps["cursor_right"]
    left = steps["cursor_left"]
    up = steps["cursor_up"]
    down = steps["cursor_down"]
    move_to(client, right["row"], right["mask"], xexpr, limits["x_max"], 600)
    move_to(client, left["row"], left["mask"], xexpr, limits["x_min"], 900)
    move_to(client, up["row"], up["mask"], yexpr, limits["y_min"], 600)
    move_to(client, down["row"], down["mask"], yexpr, limits["y_max"], 900)

    capture = client.call("screen.capture")
    require(capture["format"] == "rgba8", f"unexpected capture format: {capture['format']}")
    require(len(base64.b64decode(capture["data_base64"])) == capture["width"] * capture["height"] * 4,
            "capture has invalid RGBA8 size")
    if image_path:
        write_png(image_path, base64.b64decode(capture["data_base64"]), capture["width"], capture["height"])
    return client.call("state.hash")["hash"]


def cold_run(fixture_dir, image_path=None):
    client = Client()
    disk = (fixture_dir / "nave.dsk").read_bytes()
    symbols = (fixture_dir / "nave.noi").read_bytes()
    fixture = json.loads((fixture_dir / "fixture.json").read_text())
    client.call("disc.load", {"drive": 0, "data_base64": base64.b64encode(disk).decode()})
    client.call("symbols.load", {"data_base64": base64.b64encode(symbols).decode()})
    client.call("machine.reset")
    entry = int(fixture["entry"]["address"], 0)
    client.call("boot_ready.configure", {"address": entry})

    # Boot the ROM firmware on virtual frames, then queue the BASIC command.
    client.run_frames(200)
    client.call("input.text", {"text": "run\"nave\n"})
    status_address = fixture["symbols"]["game_status"]
    status_target = int(fixture["game_status_running"], 0)
    loaded_after = 0
    while loaded_after < 3000 and peek(client, status_address) != status_target:
        count = min(10, 3000 - loaded_after)
        client.run_frames(count)
        loaded_after += count
    require(peek(client, status_address) == status_target,
            f"NAVE did not start; {status_address} != {status_target:#04x} after {loaded_after} frames")

    saved = client.call("state.save")["data_base64"]
    final_hash = movement_sequence(client, fixture, image_path)
    client.call("state.restore", {"data_base64": saved})
    replay_hash = movement_sequence(client, fixture)
    require(final_hash == replay_hash, f"state restore replay hash differs: {final_hash} != {replay_hash}")
    total_frames = client.frames
    client.close()
    return final_hash, total_frames, dict(client.method_time)


# Reference state hash at the end of the acceptance script: FNV-1a 64 of the canonical state stream
# (state format 1) plus disc CRCs and ROM bytes, so it holds only for the ROM set pinned in roms.json.
# Format H1 (C struct image, retired) gave a8750fbe248c354e for the same run. Override with
# BLUEPIXEL_EXPECT_HASH (empty string: do not check) when testing another fixture or ROM set.
REFERENCE_HASH = "be44e742783fb94c"


def main():
    fixture_dir = Path(os.environ.get("BLUEPIXEL_FIXTURE_DIR") or Path(__file__).resolve().parents[1] / "fixtures" / "nave")
    rom_dir = Path(os.environ["BLUEPIXEL_ROM_DIR"])
    # Build first, like test_api.py, so the hash is never checked against a stale binary (CC/CFLAGS/LDFLAGS
    # pass through). BLUEPIXEL_NO_BUILD=1 tests an existing binary as it is.
    if os.environ.get("BLUEPIXEL_NO_BUILD") != "1":
        subprocess.run([str(ROOT / "tools" / "build-headless.sh")], check=True, cwd=ROOT)
    require(BINARY.exists(), "build/bluepixel-headless is missing; run tools/build-headless.sh first")
    expected = {}
    for line in (fixture_dir / "SHA256SUMS").read_text().splitlines():
        digest, filename = line.split(None, 1)
        expected[filename.lstrip("* ")] = digest
    for filename in ("nave.dsk", "nave.bin", "nave.noi", "fixture.json", "acceptance.json"):
        actual = hashlib.sha256((fixture_dir / filename).read_bytes()).hexdigest()
        require(actual == expected[filename], f"SHA-256 mismatch for {filename}")
    require((rom_dir / "roms.json").is_file(), f"missing {rom_dir / 'roms.json'}")

    start = time.perf_counter()
    with tempfile.TemporaryDirectory(prefix="bluepixel-nave-") as tmp:
        first, frames1, times1 = cold_run(fixture_dir, Path(tmp) / "final.png")
        second, frames2, times2 = cold_run(fixture_dir)
    elapsed = time.perf_counter() - start
    require(first == second, f"cold-start hash differs: {first} != {second}")
    want = os.environ.get("BLUEPIXEL_EXPECT_HASH", REFERENCE_HASH)
    require(not want or first == want, f"final state hash {first} != reference {want} (state format 1)")
    fps = (frames1 + frames2) / elapsed
    realtime = fps / 50.08
    print(f"PASS nave: hash={first} (reference {'checked' if want else 'not checked'}), cold_hash_equal=yes, restore_replay=yes")
    print(f"frames={frames1 + frames2}, wall_s={elapsed:.3f}, virtual_fps={fps:.0f}, realtime={realtime:.1f}x")
    frame_calls = times1.get("run.frames", 0.0) + times2.get("run.frames", 0.0)
    other_calls = elapsed - frame_calls
    print(f"timing: run.frames={frame_calls:.3f}s, input/memory/hash/state/capture/transport={other_calls:.3f}s")
    if elapsed >= 3:
        print("NOTE timing budget >3s on this host")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {str(error).replace(chr(10), ' ')}", file=sys.stderr)
        sys.exit(1)
