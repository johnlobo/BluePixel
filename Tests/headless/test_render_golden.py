#!/usr/bin/env python3
"""Pixel- and sample-exact regression net for the video and sound paths.

Video: machine states built into snapshots (every screen mode, random RAM and
palettes, palette and mode changed in the middle of scanlines, partial CRTC coverage), run for a few frames, then
state.hash (which covers both frame buffers). The expected hashes were produced by the unoptimised renderer, so any
speed-up of the video code must reproduce them bit for bit.

Sound: PSG tones, noise and envelopes, registers changed between runs; the hash covers the sample generation phase and
the check also hashes every sample queued by audio.read, so a faster PSG must produce the very same samples.

  python3 Tests/headless/test_render_golden.py            check against the recorded hashes
  python3 Tests/headless/test_render_golden.py --record   print the table (only from a binary known to be right)

The hashes cover the ROM bytes, so they are only valid with the firmware bundled in web/roms (the default here; BLUEPIXEL_ROM_DIR
overrides it and then the table will not match). BLUEPIXEL_BINARY overrides the executable, BLUEPIXEL_NO_BUILD=1 skips the build.
"""
import base64
import json
import os
import random
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
os.environ["BLUEPIXEL_ROM_DIR"] = os.environ.get("BLUEPIXEL_ROM_DIR") or str(root / "web" / "roms")
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


GOLDEN_VIDEO = {
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


def psg_regs(tone=(0, 0, 0), vol=(0, 0, 0), mixer=0xFF, noise=0, env_period=0, env_shape=None):
    regs = [0] * 16
    for i, period in enumerate(tone):
        regs[2 * i], regs[2 * i + 1] = period & 255, period >> 8
    regs[6], regs[7] = noise, mixer
    regs[8], regs[9], regs[10] = vol
    regs[11], regs[12] = env_period & 255, env_period >> 8
    regs[13] = 0 if env_shape is None else env_shape
    return bytes(regs)


AUDIO_SCENARIOS = [
    {"name": "tone-a", "steps": [(psg_regs(tone=(0x20, 0, 0), vol=(15, 0, 0), mixer=0x3E), 60000)]},
    {"name": "three-tones", "steps": [(psg_regs(tone=(0x31, 0x77, 0x133), vol=(10, 12, 15), mixer=0x38), 90000)]},
    {"name": "noise", "steps": [(psg_regs(vol=(15, 0, 0), mixer=0x37, noise=5), 60000)]},
    {"name": "noise-and-tone", "steps": [(psg_regs(tone=(0x45, 0, 0), vol=(14, 9, 0), mixer=0x36, noise=17), 60000)]},
    {"name": "envelope-saw", "steps": [(psg_regs(tone=(0x10, 0, 0), vol=(16, 0, 0), mixer=0x3E, env_period=40, env_shape=0x0C), 90000)]},
    {"name": "envelope-triangle", "steps": [(psg_regs(tone=(0x18, 0, 0), vol=(16, 0, 0), mixer=0x3E, env_period=25, env_shape=0x0E), 90000)]},
    {"name": "envelope-decay-hold", "steps": [(psg_regs(tone=(0x18, 0, 0), vol=(16, 16, 0), mixer=0x3C, env_period=60, env_shape=0x09), 90000)]},
    {"name": "registers-change", "steps": [
        (psg_regs(tone=(0x40, 0, 0), vol=(15, 0, 0), mixer=0x3E), 7000),
        (psg_regs(tone=(0x20, 0x80, 0), vol=(8, 15, 0), mixer=0x3C), 7001),
        (psg_regs(vol=(0, 0, 0), mixer=0xFF), 5000),
        (psg_regs(tone=(0x05, 0, 0x100), vol=(15, 0, 15), mixer=0x3A, noise=3), 9000),
    ]},
    {"name": "silence", "steps": [(psg_regs(), 60000)]},
    {"name": "frames-tones-noise", "frames": 5, "steps": [(psg_regs(tone=(0x2A, 0x51, 0x90), vol=(12, 9, 15), mixer=0x30, noise=9), 0)]},
    {"name": "frames-envelope", "frames": 5, "steps": [(psg_regs(tone=(0x14, 0, 0), vol=(16, 0, 0), mixer=0x3E, env_period=33, env_shape=0x0A), 0)]},
    # the CPU polls the PPI port B tape-input bit while a pilot tone plays and stores what it reads: reading a port must see
    # the tape exactly where it is at that microsecond
    {"name": "tape-port-poll", "tape": b"ZXTape!\x1a\x01\x14" + bytes([0x12]) + (700).to_bytes(2, "little") + (3000).to_bytes(2, "little"),
     "code": bytes.fromhex("01" "00f5" "21" "0080" "ed78" "77" "2c" "18fa"),
     "frames": 4, "steps": [(psg_regs(), 0)]},
    # tape sound: motor on, a pilot tone playing (pure tone block 0x12: 2168 T-states per pulse, 3000 pulses)
    {"name": "tape-pilot", "tape": b"ZXTape!\x1a\x01\x14" + bytes([0x12]) + (2168).to_bytes(2, "little") + (3000).to_bytes(2, "little"),
     "steps": [(psg_regs(tone=(0x30, 0, 0), vol=(9, 0, 0), mixer=0x3E), 80000)]},
]


def run_audio(scn):
    import hashlib
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

    digest = hashlib.sha256()
    for regs, nops in scn["steps"]:
        image = bytearray(base64.b64decode(call("snapshot.save", {"version": 1})["data_base64"]))
        code = scn.get("code", bytes([0x18, 0xFE]))
        image[0x100 + 0x4000:0x100 + 0x4000 + len(code)] = code
        image[0x1B] = image[0x1C] = 0
        image[0x21:0x23] = (0xBFF0).to_bytes(2, "little")
        image[0x23:0x25] = (0x4000).to_bytes(2, "little")
        image[0x5B:0x5B + 16] = regs
        image[0x43:0x43 + 18] = bytes(STD_CRTC)               # a fresh machine has no CRTC setup: frames would last a few instructions
        if scn.get("tape"):
            image[0x58] |= 0x10                                # PPI port C bit 4: the tape motor
        call("snapshot.load", {"data_base64": base64.b64encode(bytes(image)).decode()})
        if scn.get("tape"):
            call("tape.load", {"data_base64": base64.b64encode(scn["tape"]).decode(), "name": "pilot.tzx"})
            call("tape.play", {"play": True})
        if scn.get("frames"):
            call("run.frames", {"count": scn["frames"]})     # run.frames accumulates sound/tape/FDC time as debt; run.nops settles every step
        else:
            call("run.nops", {"count": nops})
        while True:                                    # every queued sample goes into the digest
            chunk = call("audio.read")
            if not chunk["frames"]:
                break
            digest.update(base64.b64decode(chunk["data_base64"]))
    result = (call("state.hash")["hash"], digest.hexdigest()[:16])
    proc.stdin.close()
    proc.wait(timeout=10)
    return "%s/%s" % result


GOLDEN_AUDIO = {
    "tone-a": "6074a4a221495a42/140f8d1b9f1d1a0b",
    "three-tones": "64e326291d495e77/9f6c97d39bc5695e",
    "noise": "8f7f402e711b3752/52aedf3184d9c209",
    "noise-and-tone": "73943fe91a1e9320/64464dd9a25c5833",
    "envelope-saw": "ffa45d22a9feacc0/357e5db11b26c8a4",
    "envelope-triangle": "724a7c596dd4ab1e/d055bcc998609502",
    "envelope-decay-hold": "59877f32974b7039/515c97320d87e172",
    "registers-change": "bfb3f30a3edf4b27/0188b784bd435c56",
    "silence": "7c266403d59a55da/a82fa4fe46e66715",
    "frames-tones-noise": "d5c64c13f34b0c6a/5dc738f4a0299939",
    "frames-envelope": "c51c0a93313ffd68/42a8d46d15e908d3",
    "tape-port-poll": "0b8f26a5480e822d/b55a236d2ab4e2dc",
    "tape-pilot": "b7caa0f5faa5b291/cdd4c9cce7dada54",
}

if __name__ == "__main__":
    video = {s["name"]: run(s) for s in scenario_list()}
    audio = {s["name"]: run_audio(s) for s in AUDIO_SCENARIOS}
    if "--record" in sys.argv:
        print(json.dumps({"video": video, "audio": audio}, indent=4))
        sys.exit(0)
    for label, got, golden in (("video", video, GOLDEN_VIDEO), ("audio", audio, GOLDEN_AUDIO)):
        bad = {k: (got[k], golden.get(k)) for k in got if got[k] != golden.get(k)}
        assert golden and not bad, f"{label} differs from the recorded hashes: {bad}"
        assert len(set(got.values())) == len(got) or label == "audio", f"every {label} scenario must differ"
    assert len({v.split("/")[1] for v in audio.values()}) >= len(audio) - 1, "the sound scenarios must differ (silence may repeat)"
    print(f"render+sound golden: PASS ({len(video)} video and {len(audio)} sound scenarios, exact against the recorded implementation)")
