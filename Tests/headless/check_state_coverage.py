#!/usr/bin/env python3
"""Proves that the canonical state serializer covers every byte of machine state.

The layout of struct CPC, struct FDC, struct CPCTape and struct CPCAutoType is read from the debug information (gdb `ptype /o`),
so a field added to any of them later is probed automatically. Every leaf field is classified:

  * NOT_STATE   listed below with the reason: live pointers, host presentation, debugger/recorder hooks.
  * COMPARED    configuration: must reach the stream (the hash and restore hazards see it), not restored.
  * everything else is machine state: it must reach the stream AND survive save -> restore.

state_probe.c flips each field's bytes and reports any byte the stream does not see. A new field that is
neither serialized nor listed here fails this check. Padding (gdb "holes") is skipped by construction.

Then it fuzzes restore with corrupted streams (state_probe.c) under ASan/UBSan.

HOSTILE_ITERATIONS=N (default 3000 per model) lengthens the hostile-value phase.

Needs gcc, gdb and zlib headers (and libasan/libubsan unless SANITIZE=0). No ROMs or media. Exit 0 = PASS.
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Fields that are not machine state, with the reason. Paths are dotted member names; a prefix covers a subtree.
NOT_STATE = {
    "cpc": {
        "z80.ctx": "callback context pointer", "z80.rd": "callback", "z80.fetch": "callback", "z80.wr": "callback",
        "z80.in": "callback", "z80.out": "callback",
        "z80.brk_enabled": "debugger setting", "z80.brk_hit": "debugger flag", "z80.turbo": "host speed setting",
        "cfg.monitor": "presentation", "cfg.linear_palette": "presentation", "cfg.sample_rate": "host audio",
        "cfg.stereo": "host audio", "cfg.volume": "host audio", "cfg.disc_sound": "host audio", "cfg.tape_sound": "host audio",
        "ram": "pointer (contents are serialized)", "ram_size": "allocation size", "rom_lower": "media pointer",
        "rom_upper": "media pointers", "rom_multiface": "pointer (contents are serialized)", "cartridge": "media pointer",
        "cartridge_banks": "media size (hashed with the media)", "rd_map": "derived, recomputed after restore",
        "wr_map": "derived, recomputed after restore", "hw_rgb": "derived palette colours (presentation)",
        "asic.pal_rgb": "derived palette colours", "asic.spal_rgb": "derived palette colours",
        "frame": "pointer (pixels are serialized)", "frame_done": "pointer (pixels are serialized)",
        "frame_ready": "host handshake flag", "audio": "host audio queue", "audio_cap": "host audio queue",
        "audio_w": "host audio queue", "audio_r": "host audio queue",
        "fdc": "pointer (FDC is serialized)", "tape": "pointer (tape position is serialized)",
        "rec_active": "host tape recording to WAV", "rec_buf": "host tape recording", "rec_len": "host tape recording",
        "rec_cap": "host tape recording", "rec_phase": "host tape recording", "t_counter": "debugger T counter",
        "dbg": "debugger", "recorder": "session recorder", "stopped_recording": "session recorder",
        "player": "session player", "trace_io": "host callback", "trace_io_ctx": "host callback",
        "autotype": "pointer (the auto-type queue is serialized by cpc_autotype_state_visit)",
    },
    "fdc": {
        "cpc": "back pointer", "disc": "media pointers", "buf": "pointer (contents are serialized)",
        "cur": "pointer (stored as an index)", "curtrk": "pointer (stored as an index)", "fmt_track": "pointer (stored as an index)",
    },
    "autotype": {"text": "pointer (the text is serialized)"},
    "tape": {
        "data": "media", "size": "COMPARED", "kind": "media format", "blocks": "media index", "nblocks": "COMPARED",
        "pulses": "derived from the current block", "npulses": "derived", "pcap": "allocation size",
        "samples": "media", "nsamples": "media", "rate": "media", "name": "media",
    },
}
# Configuration that the state carries for comparison (CPC_STATE_HAZARD_CONFIG) but does not restore.
COMPARED = {"cpc": ("model", "cfg."), "fdc": (), "tape": ("size", "nblocks"), "autotype": ()}
STRUCTS = {"cpc": "struct CPC", "fdc": "struct FDC", "tape": "struct CPCTape", "autotype": "struct CPCAutoType"}

# Address/undefined-behaviour sanitizers for the fuzz phase; SANITIZE=0 builds without (e.g. no libasan).
SANITIZE = [] if os.environ.get("SANITIZE") == "0" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]

LINE = re.compile(r"/\*\s+(\d+)\s+\|\s+(\d+)\s+\*/\s+(.*?)\s*;\s*$")


def gdb_layout(binary, type_name, cache):
    if type_name in cache:
        return cache[type_name]
    out = subprocess.run(["gdb", "-batch", "-ex", f"ptype /o {type_name}", binary],
                         capture_output=True, text=True, check=True).stdout
    members = []
    depth = 0
    for line in out.splitlines():
        if "type =" in line:
            depth = 1
            continue
        m = LINE.match(line)
        if m and depth == 1:
            off, size, decl = int(m.group(1)), int(m.group(2)), m.group(3)
            if decl.endswith("{") or "{" in decl:
                depth += 1  # anonymous nested struct/union: treat the following lines as part of it
                continue
            members.append((off, size, decl))
        elif "{" in line and depth >= 1 and not m:
            depth += line.count("{")
        if line.strip().startswith("}") and depth > 1:
            depth -= 1
    cache[type_name] = members
    return members


DECL = re.compile(r"^(?P<type>.+?)\s*(?P<ptr>\**)(?P<name>\w+)(?P<arr>(\[\d+\])*)$")
NESTED = {"Z80", "CPCConfig", "CPCAsic", "CPCCrtc", "CPCPsg", "CPCDma"}


def leaves(binary, type_name, prefix, base, cache):
    for off, size, decl in gdb_layout(binary, type_name, cache):
        fp = re.match(r"^.*\(\*(\w+)\)\(.*\)$", decl)          # function pointer member
        if fp:
            yield prefix + fp.group(1), base + off, size, False
            continue
        m = DECL.match(decl)
        if not m:
            raise SystemExit(f"cannot parse member declaration: {decl!r}")
        t, ptr, name, arr = m.group("type").strip(), m.group("ptr"), m.group("name"), m.group("arr")
        path = prefix + name
        if not ptr and t in NESTED:
            count = 1
            for d in re.findall(r"\[(\d+)\]", arr):
                count *= int(d)
            el = size // count
            for i in range(count):
                sub = f"{path}[{i}]." if count > 1 else path + "."
                yield from leaves(binary, t, sub, base + off + i * el, cache)
        else:
            yield path, base + off, size, t == "_Bool" and not ptr


def classify(obj, path):
    for k, why in NOT_STATE[obj].items():
        if (path == k or path.startswith(k + ".") or path.startswith(k + "[")) and why != "COMPARED":
            return "skip"
    if any(path == c or (c.endswith(".") and path.startswith(c)) for c in COMPARED[obj]):
        return "compared"
    return "state"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "state_probe")
        src = [os.path.join(ROOT, "Sources/CPCCore", f) for f in sorted(os.listdir(os.path.join(ROOT, "Sources/CPCCore"))) if f.endswith(".c")]
        subprocess.run(["cc", "-std=gnu11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-w", *SANITIZE,
                        "-I", os.path.join(ROOT, "Sources/CPCCore/include"), *src,
                        os.path.join(ROOT, "Tests/headless/state_probe.c"), "-lz", "-lm", "-o", probe], check=True)
        cache, lines, counts = {}, [], {"state": 0, "compared": 0, "skip": 0}
        for obj, type_name in STRUCTS.items():
            for path, off, size, is_bool in leaves(probe, type_name, "", 0, cache):
                kind = classify(obj, path)
                counts[kind] += 1
                if kind != "skip":
                    # ":bool" tells the probe to write only 0/1 there (any other byte is undefined behaviour)
                    lines.append(f"{obj} {off} {size} {1 if kind == 'state' else 0} {obj}.{path}{':bool' if is_bool else ''}")
        # every NOT_STATE entry must still name a real field, so the list cannot rot silently
        names = {obj: {p for p, _, _, _ in leaves(probe, t, "", 0, cache)} for obj, t in STRUCTS.items()}
        stale = [f"{o}.{k}" for o, ks in NOT_STATE.items() for k in ks
                 if not any(n == k or n.startswith(k + ".") or n.startswith(k + "[") for n in names[o])]
        env = dict(os.environ, ASAN_OPTIONS="exitcode=99", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=98")
        r = subprocess.run([probe], input="\n".join(lines) + "\n", capture_output=True, text=True, env=env)
    print(r.stdout, end="")
    if r.returncode not in (0, 1):
        print(f"probe crashed (exit {r.returncode}):\n" + r.stderr[-3000:])
    print(f"fields: {counts['state']} state, {counts['compared']} compared config, {counts['skip']} not state")
    if stale:
        print("stale NOT_STATE entries (field no longer exists): " + ", ".join(stale))
    ok = r.returncode == 0 and not stale
    print("state coverage: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
