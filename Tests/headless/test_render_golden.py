#!/usr/bin/env python3
"""Pixel-exact regression net for the video path: machine states built into snapshots (every screen mode, random RAM and
palettes, palette and mode changed in the middle of scanlines, partial CRTC coverage), run for a few frames, then
state.hash (which covers both frame buffers). The expected hashes were produced by the unoptimised renderer, so any
speed-up of the video code must reproduce them bit for bit.

  python3 Tests/headless/test_render_golden.py            check against the recorded hashes
  python3 Tests/headless/test_render_golden.py --record   print the table (only from a binary known to be right)

Needs BLUEPIXEL_ROM_DIR. BLUEPIXEL_BINARY overrides the executable, BLUEPIXEL_NO_BUILD=1 skips the build.
"""
import base64
import json
import os
import random
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
binary = Path(os.environ.get("BLUEPIXEL_BINARY") or root / "build" / "bluepixel-headless")
if not os.environ.get("BLUEPIXEL_BINARY") and os.environ.get("BLUEPIXEL_NO_BUILD") != "1":
    subprocess.run([str(root / "tools" / "build-headless.sh")], check=True, cwd=root, stdout=subprocess.DEVNULL)

STD_CRTC = [63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0, 0, 0, 0, 0]
# raster programs at 0x4000. Both loop forever with interrupts off.
PALETTE_RASTER = bytes.fromhex("01007f1640" "3e10ed79" "7aed79" "14" "7ae61ff640" "57" "18f0")
MODE_RASTER = bytes.fromhex("01007f1600" "7a" "e603" "f68c" "ed79" "14" "18f6")


def scenario_list():
    out = []
    for mode in range(4):
        out.append({"name": f"static-mode{mode}", "mode": mode, "seed": 100 + mode, "palette": "random"})
    out.append({"name": "palette-raster-mode1", "mode": 1, "seed": 7, "palette": "random", "code": PALETTE_RASTER})
    out.append({"name": "palette-raster-mode0", "mode": 0, "seed": 8, "palette": "random", "code": PALETTE_RASTER})
    out.append({"name": "mode-raster", "mode": 1, "seed": 9, "palette": "random", "code": MODE_RASTER})
    out.append({"name": "narrow-crtc-mode1", "mode": 1, "seed": 10, "palette": "random", "crtc": {1: 20, 2: 30}})
    out.append({"name": "short-frame-mode2", "mode": 2, "seed": 11, "palette": "random", "crtc": {6: 12, 4: 20, 7: 16}})
    out.append({"name": "no-display-rows", "mode": 1, "seed": 12, "palette": "random", "crtc": {6: 0}})
    out.append({"name": "wide-line-mode0", "mode": 0, "seed": 13, "palette": "random", "crtc": {0: 70, 1: 60, 2: 64}})
    out.append({"name": "scrolled-start-mode1", "mode": 1, "seed": 14, "palette": "random", "crtc": {12: 0x10, 13: 0x37}})
    return out


def run(scn):
    proc = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    n = 0

    def call(method, params=None):
        nonlocal n
        n += 1
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params or {}}) + "\n")
        proc.stdin.flush()
        reply = json.loads(proc.stdout.readline())
        assert "error" not in reply, (method, reply)
        return reply["result"]

    image = bytearray(base64.b64decode(call("snapshot.save", {"version": 1})["data_base64"]))
    rng = random.Random(scn["seed"])
    image[0x100:0x100 + 65536] = rng.randbytes(65536)                  # every byte of RAM, so any screen address shows something
    code = scn.get("code", bytes([0x18, 0xFE]))                          # JR $
    image[0x100 + 0x4000:0x100 + 0x4000 + len(code)] = code
    image[0x1B] = image[0x1C] = 0                                        # interrupts off
    image[0x21:0x23] = (0xBFF0).to_bytes(2, "little")
    image[0x23:0x25] = (0x4000).to_bytes(2, "little")
    image[0x40] = (image[0x40] & ~3) | scn["mode"] | 0x0C                # ROMs paged out: the random RAM is what is displayed
    for i in range(17):
        image[0x2F + i] = rng.randrange(32)
    crtc = list(STD_CRTC)
    for reg, value in scn.get("crtc", {}).items():
        crtc[reg] = value
    image[0x43:0x43 + 18] = bytes(crtc)
    call("snapshot.load", {"data_base64": base64.b64encode(bytes(image)).decode()})
    call("run.frames", {"count": 6})
    result = call("state.hash")["hash"]
    proc.stdin.close()
    proc.wait(timeout=10)
    return result


GOLDEN = {
    "static-mode0": "f34a9b4d2f1e7de1",
    "static-mode1": "28622829db4f9a53",
    "static-mode2": "bd5fd78d3bec3a9d",
    "static-mode3": "2c6ebc9721607ed6",
    "palette-raster-mode1": "f3f4e0fbdcaaea2f",
    "palette-raster-mode0": "27678817881d4777",
    "mode-raster": "421bad7c2a799ae8",
    "narrow-crtc-mode1": "2cedb987474a9652",
    "short-frame-mode2": "9d10b566da750999",
    "no-display-rows": "64bd041ba68f3d9f",
    "wide-line-mode0": "80ff650a8baf1f43",
    "scrolled-start-mode1": "1917313ab1f56866",
}

if __name__ == "__main__":
    scenarios = scenario_list()
    got = {s["name"]: run(s) for s in scenarios}
    if "--record" in sys.argv:
        print(json.dumps(got, indent=4))
        sys.exit(0)
    bad = {k: (got[k], GOLDEN.get(k)) for k in got if got[k] != GOLDEN.get(k)}
    assert GOLDEN and not bad, f"render differs from the recorded hashes: {bad}"
    assert len(set(got.values())) == len(got), "every scenario must produce a different picture"
    print(f"render golden: PASS ({len(got)} scenarios, pixel-exact against the recorded renderer)")
