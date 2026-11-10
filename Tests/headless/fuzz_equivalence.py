#!/usr/bin/env python3
"""Differential fuzz: random machines run on a reference executable and on the one under test must end in the same state.

Each seed builds a snapshot with random RAM (including random Z80 code that runs, writes to the gate array, CRTC and PSG
ports and takes interrupts), a random screen mode, palette, CRTC setup, PSG registers and interrupt mode, runs a few frames in
uneven chunks, and compares state.hash (which covers both frame buffers and the sound generation phase), the CPU registers
and a digest of every audio sample. Use it to prove a speed-up changed nothing observable.

  python3 Tests/headless/fuzz_equivalence.py --reference /path/to/old/bluepixel-headless [--seeds 200] [--start 1]

Needs BLUEPIXEL_ROM_DIR. The reference is any earlier build; CI builds one from a pinned commit.
"""
import argparse
import base64
import hashlib
import json
import os
import random
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--reference", required=True, type=Path)
parser.add_argument("--binary", type=Path, default=root / "build" / "bluepixel-headless")
parser.add_argument("--seeds", type=int, default=200)
parser.add_argument("--start", type=int, default=1)
args = parser.parse_args()


class Client:
    def __init__(self, path):
        self.proc = subprocess.Popen([str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.n = 0

    def call(self, method, params=None):
        self.n += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}}) + "\n")
        self.proc.stdin.flush()
        reply = json.loads(self.proc.stdout.readline())
        assert "error" not in reply, (method, reply)
        return reply["result"]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def machine_image(seed, base):
    rng = random.Random(seed)
    image = bytearray(base)
    image[0x100:0x100 + 65536] = rng.randbytes(65536)
    # a few hand-placed instruction sequences so that port writes really happen: OUT (C),A with BC at a GA/CRTC/PSG port
    for _ in range(rng.randrange(1, 6)):
        at = 0x4000 + rng.randrange(0, 0x3000)
        port = rng.choice([0x7F00, 0xBC00, 0xBD00, 0xF400, 0xF600, 0xF700, 0x7F10, 0x7F40])
        code = bytes([0x01, port & 255, port >> 8, 0x3E, rng.randrange(256), 0xED, 0x79, 0x18, 0xF6]) if rng.random() < 0.5 else \
            bytes([0x01, port & 255, port >> 8, 0x3E, rng.randrange(256), 0xED, 0x79])
        image[0x100 + at:0x100 + at + len(code)] = code
    image[0x23:0x25] = (0x4000).to_bytes(2, "little")                       # PC
    image[0x21:0x23] = (0xBF00 + rng.randrange(0, 0xF0)).to_bytes(2, "little")
    image[0x1B] = image[0x1C] = rng.choice([0, 1])
    image[0x25] = rng.choice([0, 1, 1, 2])
    image[0x40] = (image[0x40] & ~0x0F) | rng.randrange(4) | rng.choice([0, 4, 8, 12])
    for i in range(17):
        image[0x2F + i] = rng.randrange(32)
    crtc = [63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0, 0, 0, 0, 0]
    if rng.random() < 0.7:
        crtc[0] = rng.choice([63, 63, 63, 55, 70, 40, 100])
        crtc[1] = rng.choice([40, 40, 30, 48, 52])
        crtc[2] = rng.choice([46, 46, 50, 30, 60])
        crtc[3] = rng.choice([0x8E, 0x8E, 0x84, 0x86, 0xF8])
        crtc[4] = rng.choice([38, 38, 30, 20, 45])
        crtc[6] = rng.choice([25, 25, 20, 12, 30])
        crtc[7] = rng.choice([30, 30, 28, 24])
        crtc[9] = rng.choice([7, 7, 3, 5, 15])
        crtc[12], crtc[13] = rng.choice([(0x30, 0), (0x10, 0x30), (0x20, 0x55)]), 0
        crtc[12] = crtc[12][0] if isinstance(crtc[12], tuple) else crtc[12]
    image[0x43:0x43 + 18] = bytes(crtc)
    image[0x5B:0x5B + 16] = bytes(rng.choice([0, 0, rng.randrange(256)]) for _ in range(16))
    image[0x5B + 7] = rng.choice([0xFF, 0x38, 0x3E, 0x36, rng.randrange(256)])
    image[0x5B + 8] = rng.choice([0, 15, 16, rng.randrange(32)])
    return bytes(image), rng


def run_one(path, seed, base_image):
    client = Client(path)
    try:
        if base_image is None:
            base_image = base64.b64decode(client.call("snapshot.save", {"version": 1})["data_base64"])
        image, rng = machine_image(seed, base_image)
        client.call("snapshot.load", {"data_base64": base64.b64encode(image).decode()})
        digest = hashlib.sha256()
        for _ in range(rng.randrange(2, 5)):
            kind = rng.random()
            if kind < 0.6:
                client.call("run.frames", {"count": rng.randrange(1, 4)})
            elif kind < 0.8:
                client.call("run.nops", {"count": rng.randrange(1, 40000)})
            else:
                client.call("run.instructions", {"count": rng.randrange(1, 6000)})
            while True:
                chunk = client.call("audio.read")
                if not chunk["frames"]:
                    break
                digest.update(base64.b64decode(chunk["data_base64"]))
        regs = client.call("cpu.registers")
        return (client.call("state.hash")["hash"], digest.hexdigest()[:16], regs["pc"], regs["frames"], regs["nops"])
    finally:
        client.close()


base = None
ref0 = Client(args.reference)
base = base64.b64decode(ref0.call("snapshot.save", {"version": 1})["data_base64"])
ref0.close()
failures = []
for seed in range(args.start, args.start + args.seeds):
    expected = run_one(args.reference, seed, base)
    actual = run_one(args.binary, seed, base)
    if expected != actual:
        failures.append((seed, expected, actual))
        print(f"seed {seed}: DIFFERENT reference={expected} binary={actual}", flush=True)
distinct = len({run_one(args.reference, s, base)[0] for s in range(args.start, args.start + min(args.seeds, 20))})
assert distinct > 10, "the random machines are too alike to prove anything"
if failures:
    print(f"{len(failures)} of {args.seeds} seeds differ", file=sys.stderr)
    sys.exit(1)
print(f"fuzz equivalence: PASS ({args.seeds} random machines identical in state, registers and every audio sample)")
