#!/usr/bin/env python3
"""Canonical state is independent of struct layout: two builds with different ABIs agree byte for byte.

Builds Tests/headless/state_dump.c twice: the default layout, and every struct packed (-fpack-struct=1, no
padding, different offsets and sizes). Both run the nave fixture from a cold start to frame N (default 500)
and must produce the same canonical stream, the same state hash, and, after N more frames, the same hash.
Then each build restores the other's state file (handoff between instances) and must reach that same
final hash. It also saves and restores at every frame of the disc load (many inside FDC transfers): no
legitimate state may be rejected or restore differently. This stands in for wasm32 vs native until the WASM build exists; it proves the stream does not
depend on padding or member offsets, not on endianness (no big-endian host here).

Env: BLUEPIXEL_ROM_DIR (required), BLUEPIXEL_FIXTURE_DIR (default Tests/fixtures/nave), FRAMES (default 500).
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
rom_dir = Path(os.environ["BLUEPIXEL_ROM_DIR"])
fixture = Path(os.environ.get("BLUEPIXEL_FIXTURE_DIR", ROOT / "Tests/fixtures/nave"))
frames = os.environ.get("FRAMES", "500")
m = json.loads((rom_dir / "roms.json").read_text())["machines"]["cpc6128"]
roms = [str(rom_dir / m[k]["file"]) for k in ("os", "basic", "amsdos")]
dsk = str(fixture / "nave.dsk")

core = sorted(str(p) for p in (ROOT / "Sources/CPCCore").glob("*.c"))


def build(out, extra):
    subprocess.run(["cc", "-std=gnu11", "-D_DEFAULT_SOURCE", "-O2", "-w", *extra, "-I", str(ROOT / "Sources/CPCCore/include"),
                    *core, str(ROOT / "Tests/headless/state_dump.c"), "-lz", "-lm", "-o", out], check=True)


def run(*args):
    r = subprocess.run(list(args), capture_output=True, text=True, check=True)
    return dict(line.split(" ", 1) for line in r.stdout.splitlines())


def main():
    fails = []
    with tempfile.TemporaryDirectory() as t:
        builds = {"default": [], "packed": ["-fpack-struct=1"]}
        res = {}
        for name, flags in builds.items():
            exe = f"{t}/dump_{name}"
            build(exe, flags)
            out = run(exe, "run", *roms, dsk, frames, f"{t}/{name}")
            canon = Path(f"{t}/{name}.canon").read_bytes()
            res[name] = {"exe": exe, **out, "canon": hashlib.sha256(canon).hexdigest(), "canon_len": len(canon)}
            print(f"{name:8} frame {frames}: hash {out['hash']}  canonical sha256 {res[name]['canon'][:16]}  "
                  f"({len(canon)} bytes)  {out['layout']}  frame {2 * int(frames)}: hash {out['hash2']}")
        a, b = res["default"], res["packed"]
        if a["layout"] == b["layout"]:
            fails.append("the packed build did not change the struct layout, so this test proves nothing")
        if a["canon"] != b["canon"]:
            fails.append("canonical streams differ between layouts")
        if a["hash"] != b["hash"] or a["hash2"] != b["hash2"]:
            fails.append("state hashes differ between layouts")
        for src, dst in (("default", "packed"), ("packed", "default")):
            out = run(res[dst]["exe"], "load", *roms, dsk, f"{t}/{src}.state", frames)
            ok = out.get("hash2") == a["hash2"] and out.get("hazards") == "0"
            print(f"handoff {src} -> {dst}: hazards {out.get('hazards')}, hash after {frames} frames {out.get('hash2')} "
                  + ("(matches)" if ok else "(DIFFERS)"))
            if not ok:
                fails.append(f"handoff {src} -> {dst}")
        # Every legitimate state must restore: save at each frame of the disc load (often mid FDC transfer),
        # restore into another machine, same hash. Catches validation that rejects real states.
        ev = run(res["default"]["exe"], "every", *roms, dsk, os.environ.get("EVERY_FRAMES", "700"), "-")
        print(f"restore at every frame: {ev['saves']} saves, {ev['fdc_busy']} with the FDC busy, "
              f"{ev['rejected']} rejected, {ev['differ']} restored differently")
        if ev["rejected"] != "0" or ev["differ"] != "0":
            fails.append("a legitimate state was rejected or restored differently")
        if ev["fdc_busy"] == "0":
            fails.append("no save landed inside an FDC transfer, so FDC validation was not exercised")
    for f in fails:
        print("FAIL: " + f)
    print("state ABI independence: " + ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
