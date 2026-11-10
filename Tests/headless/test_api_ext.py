#!/usr/bin/env python3
"""API 1.3 methods: input, display, audio, tape, disc media and the debugger (debug.*).

Needs BLUEPIXEL_ROM_DIR (real or disposable ROMs: nothing here depends on what the firmware does, only on
properties that hold for any ROM). BLUEPIXEL_NO_BUILD=1 tests the existing binary as it is; BLUEPIXEL_SANITIZE=1
builds and tests an ASan/UBSan binary instead.
"""
import base64
import json
import os
import struct
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
binary = root / "build" / "bluepixel-headless"
env = dict(os.environ)
if os.environ.get("BLUEPIXEL_SANITIZE") == "1":
    # ASan/UBSan build (also checks for leaks when the process exits): catches memory errors a plain run survives
    binary = root / "build" / "bluepixel-headless-asan"
    env["CFLAGS"] = "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
if os.environ.get("BLUEPIXEL_NO_BUILD") != "1":
    subprocess.run([str(root / "tools" / "build-headless.sh"), str(binary)], check=True, cwd=root, env=env, stdout=subprocess.DEVNULL)
proc = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
_id = 0


def raw(method, params=None):
    global _id
    _id += 1
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": _id, "method": method, "params": params or {}}) + "\n")
    proc.stdin.flush()
    reply = json.loads(proc.stdout.readline())
    assert reply["id"] == _id, reply
    return reply


def call(method, params=None):
    reply = raw(method, params)
    assert "error" not in reply, (method, reply)
    return reply["result"]


def fails(method, params=None, contains=""):
    reply = raw(method, params)
    assert "error" in reply, (method, reply)
    assert contains in reply["error"]["message"], (method, reply)


def b64(data):
    return base64.b64encode(data).decode()


# ---- capabilities ----
caps = call("session.capabilities")
assert caps["api_version"] == "1.3", caps
for name in ("input", "audio", "tape", "disc", "display", "debugger"):
    assert name in caps["capabilities"], (name, caps)

# ---- run.frames reports "ran" and "break" ----
ran = call("run.frames", {"count": 2})
assert ran["ran"] == 2 and ran["break"] is False, ran

# ---- input.key / input.joystick ----
call("machine.reset")
call("boot_ready.configure", {"address": 0})
assert call("boot_ready.status")["keyboard_drained"]
call("input.key", {"key": 5 * 8 + 7, "down": True})          # SPACE
assert not call("boot_ready.status")["keyboard_drained"]
call("input.key", {"key": 5 * 8 + 7, "down": False})
assert call("boot_ready.status")["keyboard_drained"]
call("input.joystick", {"bit": 4, "down": True})              # fire 1
assert not call("boot_ready.status")["keyboard_drained"]
call("input.joystick", {"bit": 4, "down": False})
assert call("boot_ready.status")["keyboard_drained"]
# input.key and input.joystick must set exactly the matrix bit they name: same machine state as the raw input.matrix call
base = call("state.save")["data_base64"]
for method, params, row, bit in (("input.joystick", {"bit": 4}, 9, 4), ("input.joystick", {"bit": 2}, 9, 2), ("input.key", {"key": 5 * 8 + 7}, 5, 7), ("input.key", {"key": 8 * 8 + 3}, 8, 3)):
    call("state.restore", {"data_base64": base})
    call(method, dict(params, down=True))
    via_method = call("state.hash")["hash"]
    call("state.restore", {"data_base64": base})
    call("input.matrix", {"row": row, "mask": 0xFF & ~(1 << bit), "pressed": True})
    assert call("state.hash")["hash"] == via_method, (method, params)
    call("state.restore", {"data_base64": base})
    assert call("state.hash")["hash"] != via_method, "the press must change the state"
fails("input.key", {"key": 80}, "matrix position")
fails("input.key", {}, "matrix position")
fails("input.joystick", {"bit": 8}, "0..7")

# ---- display.set_monitor / screen.render ----
# The monitor type only recolours Plus machines (asic.c apply_monitor); the CPC 6128 palette comes straight from the
# gate array, so here the call must be accepted, leave the machine state alone and not disturb the capture.
colour = call("screen.capture", {"metadata": True})["metadata"]["palette_argb"]
hash_before = call("state.hash")["hash"]
for monitor in (1, 2, 0):
    call("display.set_monitor", {"monitor": monitor})
assert call("state.hash")["hash"] == hash_before, "monitor type is configuration, not machine state"
assert call("screen.capture", {"metadata": True})["metadata"]["palette_argb"] == colour
fails("display.set_monitor", {"monitor": 3}, "monitor must be")
full = call("screen.render")
assert (full["width"], full["height"]) == (768, 540) and len(base64.b64decode(full["data_base64"])) == 768 * 540 * 4
half = call("screen.render", {"half_size": True, "pal": False, "scanlines": False, "brightness": 20})
assert (half["width"], half["height"]) == (384, 270) and len(base64.b64decode(half["data_base64"])) == 384 * 270 * 4
fails("screen.render", {"brightness": 500}, "brightness")

# ---- audio ----
# Machine time drives the sample count (1 us per NOP, 44100 samples per second), independent of the firmware and of
# how long a CRTC frame is, so it can be asserted with disposable ROMs too.
call("machine.reset")
while call("audio.read")["frames"]:
    pass
assert call("audio.status")["available"] == 0
call("run.nops", {"count": 60000})                            # 60000 us of machine time = about 2646 samples at 44.1 kHz
status = call("audio.status")
assert status["sample_rate"] == 44100 and status["channels"] == 2 and status["format"] == "f32le"
available = status["available"]
assert 2500 < available < 2800, status
chunk = call("audio.read", {"frames": 1000})
assert chunk["frames"] == 1000 and chunk["remaining"] == available - 1000, (chunk["frames"], chunk["remaining"], available)
assert chunk["sample_rate"] == 44100 and chunk["channels"] == 2 and chunk["format"] == "f32le"
pcm = base64.b64decode(chunk["data_base64"])
assert len(pcm) == 1000 * 2 * 4
samples = struct.unpack("<2000f", pcm)
assert all(-1.5 <= v <= 1.5 for v in samples), "samples must be finite and in range"
assert call("audio.read", {"frames": 0})["frames"] == 0
assert call("audio.status")["available"] == available - 1000, "a zero-frame read must not consume"
capped = call("audio.read", {"frames": 100000})              # one call never returns more than 8192 frames
assert 0 < capped["frames"] <= 8192, capped["frames"]
call("run.nops", {"count": 2000000})                          # more than the ring holds (16383): oldest-first, no crash
assert call("audio.status")["available"] <= 16383

# ---- tape ----
assert call("tape.status")["loaded"] is False
fails("tape.play", {}, "no tape")
fails("tape.rewind", {}, "no tape")
fails("tape.load", {"data_base64": b64(b"not a tape")}, "tape")
fails("tape.load", {}, "data_base64")
cdt = b"ZXTape!\x1a\x01\x20" + b"\x20\xe8\x03"                 # header + pause block (1 s)
call("tape.load", {"data_base64": b64(cdt), "name": "demo.cdt"})
st = call("tape.status")
assert st["loaded"] and st["name"] == "demo.cdt" and not st["playing"], st
call("tape.play", {"play": True})
assert call("tape.status")["playing"]
call("tape.play", {"play": False})
assert not call("tape.status")["playing"]
call("tape.rewind")
call("tape.remove")
assert call("tape.status")["loaded"] is False
call("tape.remove")                                           # removing nothing is fine

# ---- rom.unload ----
assert call("session.capabilities")["roms_loaded"] == {"os": True, "basic": True, "amsdos": True}
rom = (Path(os.environ["BLUEPIXEL_ROM_DIR"]) / json.loads((Path(os.environ["BLUEPIXEL_ROM_DIR"]) / "roms.json").read_text())["machines"]["cpc6128"]["amsdos"]["file"]).read_bytes()
hash_with_rom = call("state.hash")["hash"]
call("rom.unload", {"role": "amsdos"})
assert call("session.capabilities")["roms_loaded"]["amsdos"] is False
assert call("state.hash")["hash"] != hash_with_rom, "state.hash covers the loaded ROM bytes"
call("rom.load", {"role": "amsdos", "data_base64": b64(rom)})
assert call("session.capabilities")["roms_loaded"]["amsdos"] is True
assert call("state.hash")["hash"] == hash_with_rom, "reloading the same ROM restores the hash"
fails("rom.unload", {"role": "bogus"}, "role must be")

# ---- disc: info and export ----
blank = bytearray(256 + 0x1300)
blank[:34] = b"MV - CPCEMU Disk-File\r\nDisk-Info\r\n"
blank[0x30], blank[0x31] = 1, 1
blank[0x32:0x34] = (0x1300).to_bytes(2, "little")
blank[0x100:0x10C] = b"Track-Info\r\n"
blank[0x114], blank[0x115], blank[0x116], blank[0x117] = 2, 9, 0x4E, 0xE5
for sector in range(9):
    d = 0x118 + sector * 8
    blank[d:d + 4] = bytes([0, 0, 0xC1 + sector, 2])
    blank[0x200 + sector * 512:0x200 + (sector + 1) * 512] = bytes([0xE5]) * 512
assert call("disc.info", {"drive": 0}) == {"drive": 0, "loaded": False, "name": ""}
fails("disc.export", {"drive": 0}, "no disc")
fails("disc.info", {"drive": 2}, "drive must be")
call("disc.load", {"drive": 0, "data_base64": b64(bytes(blank)), "name": "game \"A\".dsk"})
assert call("disc.info", {"drive": 0}) == {"drive": 0, "loaded": True, "name": 'game "A".dsk'}
exported = call("disc.export", {"drive": 0})
image = base64.b64decode(exported["data_base64"])
assert exported["name"] == 'game "A".dsk' and image.startswith((b"MV - CPCEMU", b"EXTENDED CPC DSK")), image[:16]
call("disc.load", {"drive": 1, "data_base64": exported["data_base64"]})
assert call("disc.export", {"drive": 1})["data_base64"] == exported["data_base64"], "export must round-trip"
assert call("disc.info", {"drive": 1})["name"] == "<api>"

# ---- debugger ----
assert call("debug.status")["attached"] is False
st = call("debug.attach")
assert st["attached"] and st["enabled"] and not st["break"], st

# symbols
call("debug.symbols.clear")
call("debug.symbols.set", {"name": "player_x", "value": 0x4000})
call("debug.symbols.set", {"name": "PLAYER_Y", "value": 0x4001})
call("debug.symbols.set", {"name": "player_x", "value": 0x4010})     # case-insensitive update, not a duplicate
symbols = {s["name"]: s["value"] for s in call("debug.symbols.list")["symbols"]}
assert symbols == {"player_x": 0x4010, "PLAYER_Y": 0x4001}, symbols
call("mem.write", {"address": 0x4010, "value": 0x77})
assert call("mem.read", {"address": "player_x", "length": 1})["hex"] == "77"       # symbols resolve in addresses
assert call("debug.mem.read", {"address": "player_x", "length": 1})["hex"] == "77"
fails("debug.symbols.set", {"value": 1}, "name")
fails("debug.symbols.set", {"name": "x" * 60, "value": 1}, "too long")
call("debug.symbols.clear")
assert call("debug.symbols.list")["symbols"] == []

# memory through the debugger view
call("debug.mem.write", {"address": 0x4000, "value": 0x5A})
assert call("debug.mem.read", {"address": 0x4000, "length": 2})["hex"].startswith("5a")
assert call("mem.read", {"address": 0x4000, "length": 1})["hex"] == "5a"
fails("debug.mem.read", {"address": 0x10000}, "invalid")
fails("debug.mem.write", {"address": 1, "value": 256}, "invalid")

# disassembly: addresses advance by the reported lengths
dis = call("debug.disassemble", {"address": 0, "count": 5})["instructions"]
assert len(dis) == 5 and dis[0]["address"] == 0
for first, second in zip(dis, dis[1:]):
    assert second["address"] == first["address"] + first["length"], dis
assert all(1 <= i["length"] <= 4 and i["text"] for i in dis), dis
fails("debug.disassemble", {"address": 0, "count": 0}, "invalid")
fails("debug.disassemble", {"address": 0, "count": 1000}, "invalid")

# stepping and code breakpoints; the reset makes the path from PC 0 repeatable
call("machine.reset")
pc1 = call("debug.step_into")["pc"]
pc2 = call("debug.step_into")["pc"]
assert call("cpu.registers")["pc"] == pc2
call("machine.reset")
toggled = call("debug.breakpoints.toggle_code", {"address": pc1})
assert toggled == {"address": pc1, "set": True}, toggled
listed = call("debug.breakpoints.list")["breakpoints"]
assert [(b["type"], b["start"], b["enabled"]) for b in listed] == [("code", pc1, True)], listed
call("debug.resume")
hit = call("run.frames", {"count": 3})
# a frame can end on the very instruction before the breakpoint (the check runs when the next one starts), so it may
# take a second frame to report it; it must stop at the first frame that reaches it and not run on
assert hit["break"] is True and hit["ran"] <= 2 and hit["pc"] == pc1, hit
st = call("debug.status")
assert st["break"] and st["pc"] == pc1 and st["break_index"] == listed[0]["id"], st
# stepping off the breakpoint clears the stop; resume then runs on without re-hitting it at once
call("debug.resume")
assert call("debug.status")["break"] is False
assert call("debug.breakpoints.toggle_code", {"address": pc1})["set"] is False
assert call("debug.breakpoints.list")["breakpoints"] == []

# run_to stops at an address
call("machine.reset")
call("debug.run_to", {"address": pc2})
reached = call("run.frames", {"count": 3})
assert reached["break"] and reached["pc"] == pc2, reached
fails("debug.run_to", {"address": 70000}, "invalid")

# memory breakpoints, conditions, removal
bp = call("debug.breakpoints.add_memory", {"start": 0xC000, "end": 0xC0FF, "access": "write"})["id"]
ids = {b["id"]: b for b in call("debug.breakpoints.list")["breakpoints"]}
assert ids[bp]["type"] == "mem_write" and (ids[bp]["start"], ids[bp]["end"]) == (0xC000, 0xC0FF), ids
rd = call("debug.breakpoints.add_memory", {"start": 0x8000, "end": 0x8000, "access": "read"})["id"]
assert {b["id"]: b["type"] for b in call("debug.breakpoints.list")["breakpoints"]}[rd] == "mem_read"
fails("debug.breakpoints.add_memory", {"start": 5, "end": 1}, "invalid")
fails("debug.breakpoints.add_memory", {"start": 1, "end": 2, "access": "x"}, "access")
ok = call("debug.breakpoints.set_condition", {"id": bp, "condition": "a == 1", "pass_count": 2})
assert ok == {"set": True, "error": ""}, ok
cond = {b["id"]: b for b in call("debug.breakpoints.list")["breakpoints"]}[bp]
assert cond["condition"] == "a == 1" and cond["pass_count"] == 2, cond
bad = call("debug.breakpoints.set_condition", {"id": bp, "condition": "nonsense(((", "pass_count": 0})
assert bad["set"] is False and bad["error"], bad
fails("debug.breakpoints.set_condition", {"id": 250, "condition": ""}, "no breakpoint")
call("debug.breakpoints.remove", {"id": bp})
call("debug.breakpoints.remove", {"id": rd})
assert call("debug.breakpoints.list")["breakpoints"] == []
fails("debug.breakpoints.remove", {"id": bp}, "no breakpoint")

# data areas show up as data in the disassembly
call("debug.data.mark", {"start": 0, "end": 3})
marked = call("debug.disassemble", {"address": 0, "count": 1})["instructions"][0]["text"].lower()
assert "db" in marked or "defb" in marked or "byte" in marked, marked
call("debug.data.clear", {"address": 0})
fails("debug.data.mark", {"start": 9, "end": 1}, "invalid")

# step_out and pause are accepted; detach drops the debugger
assert call("debug.step_out") is True and call("debug.pause") is True
assert call("debug.detach")["attached"] is False

# a machine with the debugger attached but nothing set must keep running whole frames
call("machine.reset")
assert call("run.frames", {"count": 3}) ["ran"] == 3

# ---- regression: a request with exactly 8192 JSON tokens used to write one slot past the parser's token array ----
big = '{"jsonrpc":"2.0","id":1,"method":"session.capabilities","params":[' + ",".join(["1"] * 8183) + "]}\n"
proc.stdin.write(big)
proc.stdin.flush()
reply = json.loads(proc.stdout.readline())
assert reply.get("result", {}).get("api_version") == "1.3", str(reply)[:200]

# ---- machine code in RAM, built into a v1 snapshot: exact control over what executes ----
# 4000 CALL 4010 / 4003 LD A,(8000) / 4006 LD (C000),A / 4009 JR $ ;  4010 NOP / 4011 NOP / 4012 RET
PROGRAM = {0x4000: bytes([0xCD, 0x10, 0x40, 0x3A, 0x00, 0x80, 0x32, 0x00, 0xC0, 0x18, 0xFE]), 0x4010: bytes([0x00, 0x00, 0xC9])}


def load_program(psg=None, tape_motor=False):
    """Machine at 4000 with interrupts off, SP in RAM; optionally a PSG voice (tone A, volume 15) and the tape motor on."""
    image = bytearray(base64.b64decode(call("snapshot.save", {"version": 1})["data_base64"]))
    image[0x1B] = image[0x1C] = 0                                       # IFF1/IFF2 off: nothing but our code runs
    image[0x21:0x23] = (0xBFF0).to_bytes(2, "little")
    image[0x23:0x25] = (0x4000).to_bytes(2, "little")
    for address, code in PROGRAM.items():
        image[0x100 + address:0x100 + address + len(code)] = code
    if psg:
        image[0x5B:0x5B + 16] = psg
    if tape_motor:
        image[0x58] |= 0x10
    call("snapshot.load", {"data_base64": b64(bytes(image))})
    assert call("cpu.registers")["pc"] == 0x4000


def clear_breakpoints():
    for bp in call("debug.breakpoints.list")["breakpoints"]:
        call("debug.breakpoints.remove", {"id": bp["id"]})


TONE_A = bytes([0x20, 0x00, 0, 0, 0, 0, 0, 0x3E, 15, 0, 0, 0, 0, 0, 0, 0])   # mixer 3E: only tone A, volume 15

load_program()
# symbols.load leaves breakpoints disabled (headless runs stay fast); debug.attach must turn them back on
call("symbols.load", {"data_base64": b64(b"DEF loop_top 0x4003\n")})
assert call("debug.status")["enabled"] is False
assert call("debug.attach")["enabled"] is True
clear_breakpoints()
call("debug.breakpoints.toggle_code", {"address": "loop_top"})
assert call("run.frames", {"count": 3})["break"], "a breakpoint must fire after symbols.load + debug.attach"
call("debug.resume")
clear_breakpoints()
load_program()

# step into a CALL, then step_out must stop right after the RET
assert call("debug.step_into")["pc"] == 0x4010
assert call("debug.step_into")["pc"] == 0x4011
call("debug.step_out")
out = call("run.frames", {"count": 1})
assert out["break"] and out["pc"] == 0x4003, out

# resume steps off a breakpoint instead of re-hitting it; a second run without resume stays stopped on it
call("debug.breakpoints.toggle_code", {"address": 0x4003})
stopped = call("run.frames", {"count": 1})
assert stopped["break"] and stopped["pc"] == 0x4003, stopped
assert call("run.frames", {"count": 1})["break"], "without debug.resume the stop is still pending"
call("debug.resume")
moved = call("run.frames", {"count": 1})
assert not moved["break"] and moved["pc"] != 0x4003, moved
clear_breakpoints()

# pause cancels an armed run-to
load_program()
call("debug.run_to", {"address": 0x4006})
call("debug.pause")
assert not call("run.frames", {"count": 3})["break"], "pause must cancel the pending run-to"
load_program()
call("debug.run_to", {"address": 0x4006})
_r = call("run.frames", {"count": 3}); assert _r["pc"] == 0x4006 and _r["break"], _r

# machine.reset clears a pending stop
load_program()
call("debug.breakpoints.toggle_code", {"address": 0x4003})
assert call("run.frames", {"count": 1})["break"] and call("debug.status")["break"]
call("machine.reset")
assert call("debug.status")["break"] is False, "machine.reset must clear the debugger stop"
clear_breakpoints()

# memory breakpoint access modes: the program reads 8000 and writes C000
def runs_into(access, address):
    load_program()
    clear_breakpoints()
    ids = call("debug.breakpoints.add_memory", {"start": address, "end": address, "access": access})
    result = call("run.frames", {"count": 3})        # a frame can end before the program reaches the access
    call("debug.resume")
    return result["break"], ids

assert runs_into("read", 0x8000)[0] is True
assert runs_into("write", 0x8000)[0] is False, "a write breakpoint must not fire on a read"
assert runs_into("write", 0xC000)[0] is True
assert runs_into("read", 0xC000)[0] is False, "a read breakpoint must not fire on a write"
fired, ids = runs_into("readwrite", 0x8000)
assert fired is True, "readwrite must fire on a read"
assert len(ids["ids"]) == 2 and ids["id"] == ids["ids"][0], ids
assert sorted(b["type"] for b in call("debug.breakpoints.list")["breakpoints"]) == ["mem_read", "mem_write"]
assert runs_into("readwrite", 0xC000)[0] is True, "readwrite must fire on a write"
clear_breakpoints()

# a full table is all or nothing: a readwrite pair never half-fits
for _ in range(255):
    call("debug.breakpoints.add_memory", {"start": 1, "end": 1})
assert len(call("debug.breakpoints.list")["breakpoints"]) == 255
fails("debug.breakpoints.add_memory", {"start": 2, "end": 2, "access": "readwrite"}, "full")
assert len(call("debug.breakpoints.list")["breakpoints"]) == 255, "a refused readwrite must not leave half a pair behind"
call("debug.breakpoints.add_memory", {"start": 3, "end": 3})
fails("debug.breakpoints.add_memory", {"start": 4, "end": 4}, "full")
clear_breakpoints()

# the words flag changes how a data area is disassembled
call("debug.data.mark", {"start": 0x4010, "end": 0x4013, "words": False})
as_bytes = call("debug.disassemble", {"address": 0x4010, "count": 1})["instructions"][0]
call("debug.data.clear", {"address": 0x4010})
call("debug.data.mark", {"start": 0x4010, "end": 0x4013, "words": True})
as_words = call("debug.disassemble", {"address": 0x4010, "count": 1})["instructions"][0]
call("debug.data.clear", {"address": 0x4010})
assert as_bytes["text"] != as_words["text"] and as_words["length"] == 2, (as_bytes, as_words)

# conditions: the debugger stores 159 bytes, so a longer one is refused, not silently truncated into a different test
bp = call("debug.breakpoints.add_memory", {"start": 5, "end": 5})["id"]
fails("debug.breakpoints.set_condition", {"id": bp, "condition": "1" + " " * 170 + "* 0"}, "too long")
assert call("debug.breakpoints.set_condition", {"id": bp, "condition": "1" + " " * 158})["set"] is True
clear_breakpoints()

# symbol names: JSON escapes are decoded (\u0041 is A, a surrogate pair is one astral character), bad ones refused
call("debug.symbols.clear")
call("debug.symbols.set", {"name": "a\u0041b", "value": 1})
call("debug.symbols.set", {"name": "\ud83d\ude00", "value": 2})
assert {s["name"]: s["value"] for s in call("debug.symbols.list")["symbols"]} == {"aAb": 1, "\U0001F600": 2}
proc.stdin.write('{"jsonrpc":"2.0","id":9001,"method":"debug.symbols.set","params":{"name":"\\ud83d","value":3}}\n')
proc.stdin.flush()
assert "name" in json.loads(proc.stdout.readline())["error"]["message"], "a lone surrogate must be refused"
call("debug.symbols.clear")
for i in range(16384):
    call("debug.symbols.set", {"name": f"s{i}", "value": i})
fails("debug.symbols.set", {"name": "one_too_many", "value": 0}, "full")
call("debug.symbols.set", {"name": "s0", "value": 99})            # updating an existing name still works when full
call("debug.symbols.clear")

# audio with a real signal: byte order, and the per-call cap
load_program(psg=TONE_A)
while call("audio.read")["frames"]:                                   # the ring drops new samples while full: empty it first
    pass
call("run.nops", {"count": 400000})                                   # more than 8192 frames queued
avail = call("audio.status")["available"]
assert avail > 8192, avail
chunk = call("audio.read", {"frames": 100000})
assert chunk["frames"] == 8192, "one audio.read call returns at most 8192 frames"
pcm = base64.b64decode(chunk["data_base64"])
little = struct.unpack("<%df" % (len(pcm) // 4), pcm)
assert max(abs(v) for v in little) > 0.05, "tone A at volume 15 must be audible"
assert max(abs(v) for v in little) < 1.5
big = struct.unpack(">%df" % (len(pcm) // 4), pcm)
assert max(abs(v) for v in big) > 1e3 or any(v != v for v in big), "samples must really be little-endian"

# screen.render without filtering is the capture, doubled vertically, channel for channel
call("run.frames", {"count": 3})
cap = base64.b64decode(call("screen.capture")["data_base64"])
ren = base64.b64decode(call("screen.render", {"pal": False, "scanlines": True, "half_size": False, "brightness": 0})["data_base64"])
row = 768 * 4
assert all(ren[(2 * y) * row:(2 * y + 1) * row] == cap[y * row:(y + 1) * row] == ren[(2 * y + 1) * row:(2 * y + 2) * row] for y in range(270)), \
    "screen.render (no PAL, scanlines on) must equal screen.capture"

# tape: with the motor on and playing, the position advances; rewind takes it back to the start
load_program(tape_motor=True)
call("tape.load", {"data_base64": b64(b"ZXTape!\x1a\x01\x20" + b"\x20\xd0\x07"), "name": "long.cdt"})   # 2 s pause block
call("tape.play", {"play": True})
call("run.nops", {"count": 100000})
advanced = call("tape.status")["position_seconds"]
assert advanced > 0.05, advanced
call("tape.rewind")
assert call("tape.status")["position_seconds"] == 0.0
call("tape.remove")

# ---- hostile and boundary input ----
def raw_line(text):
    proc.stdin.write(text + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())


# expressions nest recursively: depth is capped so a request cannot exhaust the stack (a WebAssembly stack is small)
assert call("mem.read", {"address": "(" * 10 + "1" + ")" * 10})["address"] == 1
for depth in (200, 20000):
    fails("mem.read", {"address": "(" * depth + "1" + ")" * depth}, "invalid memory range")
fails("mem.read", {"address": "-" * 20000 + "1"}, "invalid memory range")
assert call("session.capabilities")["api_version"] == "1.3", "the process survived the deep expressions"

# \u0000 would end a C string early: refused everywhere instead of silently truncating
fails("debug.symbols.set", {"name": "ab\u0000cd", "value": 1}, "name")
fails("debug.breakpoints.set_condition", {"id": call("debug.breakpoints.add_memory", {"start": 9, "end": 9})["id"], "condition": "pc==1\u0000junk((("}, "required")
fails("mem.read", {"address": "ab\u0000cd"}, "invalid")
clear_breakpoints()

# every JSON string escape decodes to the right text; code points of 1, 2, 3 and 4 UTF-8 bytes; uppercase hex
call("debug.symbols.clear")
cases = {"t\tx": 1, "n\nx": 2, "b\bx": 3, "f\fx": 4, "s/x": 5, 'q"x': 6, "bs\\x": 7, "é": 8, "€": 9, "\U0010ffff": 10, "éÉ": 11, "ñÿ": 12}
wire = {"t\tx": "t\\tx", "n\nx": "n\\nx", "b\bx": "b\\bx", "f\fx": "f\\fx", "s/x": "s\\/x", 'q"x': 'q\\"x', "bs\\x": "bs\\\\x",
        "é": "\\u00e9", "€": "\\u20ac", "\U0010ffff": "\\udbff\\udfff", "éÉ": "\\u00E9\\u00c9", "ñÿ": "\\u00F1\\u00fF"}
for name, value in cases.items():
    reply = raw_line('{"jsonrpc":"2.0","id":1,"method":"debug.symbols.set","params":{"name":"%s","value":%d}}' % (wire[name], value))
    assert reply.get("result") is True, (name, reply)
listed = {s["name"]: s["value"] for s in call("debug.symbols.list")["symbols"]}
assert listed == cases, (listed, cases)
call("debug.symbols.clear")
for bad in ("\\udc00", "\\ud800", "\\ud800\\ue000", "\\ud800\\u0041"):       # lone low, lone high, high + non-surrogate
    reply = raw_line('{"jsonrpc":"2.0","id":2,"method":"debug.symbols.set","params":{"name":"%s","value":1}}' % bad)
    assert "error" in reply, (bad, reply)
assert "error" in raw_line('{"jsonrpc":"2.0","id":3,"method":"debug.symbols.set","params":{"name":"a\\u12","value":1}}'), "truncated \\u escape"

# replies are always valid UTF-8 JSON: a Latin-1 symbol map comes back with U+FFFD, not a raw 0xE9 byte
call("symbols.load", {"data_base64": b64(b"DEF caf\xe9 0x4000\n")})
assert [s["name"] for s in call("debug.symbols.list")["symbols"]] == ["caf�"]
call("debug.symbols.clear")

# exact boundaries of every limit
call("debug.symbols.set", {"name": "n" * 47, "value": 2147483647})
fails("debug.symbols.set", {"name": "n" * 48, "value": 1}, "too long")
call("debug.symbols.set", {"name": "lo", "value": -2147483648})
fails("debug.symbols.set", {"name": "hi", "value": 2147483648}, "value")
fails("debug.symbols.set", {"name": "lo2", "value": -2147483649}, "value")
call("debug.symbols.clear")
bp = call("debug.breakpoints.add_memory", {"start": 5, "end": 5})["id"]
assert call("debug.breakpoints.set_condition", {"id": bp, "condition": "1" + " " * 158})["set"]          # 159 bytes
fails("debug.breakpoints.set_condition", {"id": bp, "condition": "1" + " " * 159}, "too long")          # 160 bytes
assert call("debug.breakpoints.set_condition", {"id": bp, "condition": "", "pass_count": 1000000})["set"]
fails("debug.breakpoints.set_condition", {"id": bp, "condition": "", "pass_count": 1000001}, "pass_count")
fails("debug.breakpoints.set_condition", {"id": bp, "condition": "", "pass_count": -1}, "pass_count")
clear_breakpoints()
assert len(call("debug.disassemble", {"address": 0, "count": 256})["instructions"]) == 256
fails("debug.disassemble", {"address": 0, "count": 257}, "invalid")
assert len(call("debug.mem.read", {"address": 0, "length": 4096})["hex"]) == 8192
fails("debug.mem.read", {"address": 0, "length": 4097}, "invalid")
assert len(call("debug.mem.read", {"address": 0xFFFF, "length": 1})["hex"]) == 2
fails("debug.mem.read", {"address": 0xFFFF, "length": 2}, "invalid")        # no wrap past 64K
fails("mem.read", {"address": 0xFFFF, "length": 2}, "invalid")

# a state-changing parameter that is missing, mistyped or fractional is an error, never a made-up value
fails("debug.symbols.set", {"name": "v"}, "value")
fails("debug.symbols.set", {"name": "v", "value": "zz"}, "value")
fails("debug.symbols.set", {"name": "v", "value": 1.5}, "value")
fails("debug.mem.write", {"address": 0x4000}, "invalid")
fails("debug.breakpoints.add_memory", {"start": 1, "end": 1, "access": 5}, "access")
bp = call("debug.breakpoints.add_memory", {"start": 6, "end": 6})["id"]
call("debug.breakpoints.set_condition", {"id": bp, "condition": "1", "pass_count": 3})
fails("debug.breakpoints.set_condition", {"id": bp, "condition": 5}, "required")
fails("debug.breakpoints.set_condition", {"id": "x"}, "required")
kept = {b["id"]: b for b in call("debug.breakpoints.list")["breakpoints"]}[bp]
assert kept["condition"] == "1" and kept["pass_count"] == 3, "a rejected call must not touch the breakpoint"
fails("debug.breakpoints.remove", {"id": 1.5}, "integer")
clear_breakpoints()
fails("disc.info", {"drive": "x"}, "drive")
fails("audio.read", {"frames": -5}, "non-negative")

# the debugger attaches only when something needs it, and the history ring stays off
call("debug.detach")
assert call("debug.status")["attached"] is False
call("debug.symbols.list")
call("debug.breakpoints.list")
fails("debug.nope", {}, "method not found")
assert call("debug.status")["attached"] is False, "read-only and unknown debug.* calls must not attach"
assert call("mem.read", {"address": "0x10", "length": 1})["address"] == 0x10         # a symbolic address needs the debugger
status = call("debug.status")
assert status["attached"] is True and status["history"] is False, status
call("debug.detach")
assert call("debug.attach")["history"] is False
call("debug.step_into")
assert call("debug.status")["history"] is False, "no call leaves the execution history ring running"

# rom.unload really unloads the role it names (state.hash covers the ROM bytes)
roms = json.loads((Path(os.environ["BLUEPIXEL_ROM_DIR"]) / "roms.json").read_text())["machines"]["cpc6128"]
full_hash = call("state.hash")["hash"]
for role in ("os", "basic", "amsdos"):
    call("rom.unload", {"role": role})
    assert call("session.capabilities")["roms_loaded"][role] is False
    assert call("state.hash")["hash"] != full_hash, role
    call("rom.load", {"role": role, "data_base64": b64((Path(os.environ["BLUEPIXEL_ROM_DIR"]) / roms[role]["file"]).read_bytes())})
    assert call("state.hash")["hash"] == full_hash, f"reloading {role} must restore the exact state"

# defaults of the boolean knobs: a press, a play, and a name
base = call("state.save")["data_base64"]
call("input.joystick", {"bit": 0})
pressed = call("state.hash")["hash"]
call("state.restore", {"data_base64": base})
call("input.joystick", {"bit": 0, "down": True})
assert call("state.hash")["hash"] == pressed, "down defaults to true"
call("state.restore", {"data_base64": base})
call("tape.load", {"data_base64": b64(b"ZXTape!\x1a\x01\x20" + b"\x20\xe8\x03")})
assert call("tape.status")["name"] == "tape.cdt"
call("tape.play")
assert call("tape.status")["playing"] is True, "play defaults to true"
call("tape.remove")

# screen.render: scanlines off leaves the second line of each pair black; brightness -100 blacks everything out
cap = base64.b64decode(call("screen.capture")["data_base64"])
dark = base64.b64decode(call("screen.render", {"pal": False, "scanlines": False})["data_base64"])
row = 768 * 4
assert all(dark[(2 * y) * row:(2 * y + 1) * row] == cap[y * row:(y + 1) * row] for y in range(270)), "even lines are the capture"
assert all(dark[(2 * y + 1) * row:(2 * y + 2) * row] == b"\x00\x00\x00\xff" * 768 for y in range(270)), "odd lines are black"
black = base64.b64decode(call("screen.render", {"brightness": -100})["data_base64"])
assert black == b"\x00\x00\x00\xff" * (768 * 540), "brightness -100 is black"
bright = base64.b64decode(call("screen.render", {"pal": False, "scanlines": True, "brightness": 100})["data_base64"])
assert bright != base64.b64decode(call("screen.render", {"pal": False, "scanlines": True, "brightness": 0})["data_base64"]) or set(cap) == {0, 255}, "brightness must change the image"

# ---- more edges: UTF-8 validation of replies, base64 strictness, stale step-out, reset, render defaults, wrap ----
def symbol_names_after_load(blob):
    call("debug.symbols.clear")
    call("symbols.load", {"data_base64": b64(blob)})
    names = [s["name"] for s in call("debug.symbols.list")["symbols"]]
    call("debug.symbols.clear")
    return names


assert symbol_names_after_load(b"DEF a\xe0\x80\x80b 0x4000\n") == ["a���b"], "overlong 3-byte sequence"
assert symbol_names_after_load(b"DEF a\xed\xa0\x80b 0x4000\n") == ["a���b"], "UTF-8 encoded surrogate"
assert symbol_names_after_load("DEF a\U0010ffffb 0x4000\n".encode()) == ["a\U0010ffffb"], "the largest code point is valid"
assert symbol_names_after_load(b"DEF a\xf4\x90\x80\x80b 0x4000\n") == ["a����b"], "beyond U+10FFFF"
assert symbol_names_after_load("DEF aé€b 0x4000\n".encode()) == ["aé€b"], "valid 2- and 3-byte sequences pass through"

# malformed base64 is refused with a base64 error; correctly padded 1-, 2- and 3-byte payloads decode (then fail as a tape)
for bad in ("A===", "AA=A", "AAAAA", "AAA", "====", "A=AA", "AA A", "AA\n=="):
    fails("tape.load", {"data_base64": bad}, "data_base64")
for good in ("AA==", "AAA=", "AAAA"):
    fails("tape.load", {"data_base64": good}, "tape image")

# brightness bounds are exactly -100..100
call("screen.render", {"brightness": 100})
call("screen.render", {"brightness": -100})
fails("screen.render", {"brightness": 101}, "brightness")
fails("screen.render", {"brightness": -101}, "brightness")

# step-out must not survive pause, resume or a machine reset
def armed_step_out():
    load_program()
    clear_breakpoints()
    call("debug.resume")
    assert call("debug.step_into")["pc"] == 0x4010
    assert call("debug.step_into")["pc"] == 0x4011
    call("debug.step_out")


armed_step_out()
call("debug.pause")
assert not call("run.frames", {"count": 3})["break"], "pause must cancel step-out"
armed_step_out()
call("debug.resume")
assert not call("run.frames", {"count": 3})["break"], "resume must cancel step-out"
armed_step_out()
call("machine.reset")
load_program()
assert not call("run.frames", {"count": 3})["break"], "machine.reset must cancel step-out"
call("debug.resume")

# after machine.reset a breakpoint on the very first instruction does not stop the machine before it runs anything
assert call("debug.attach")["enabled"] is True      # the symbols.load calls above left breakpoints disabled
call("run.frames", {"count": 2})        # use up the skip-once flag the resume above left pending, so only the reset can provide it
call("machine.reset")
clear_breakpoints()
call("debug.breakpoints.toggle_code", {"address": 0})
call("machine.reset")
first = call("run.frames", {"count": 1})
assert first["pc"] != 0, first
clear_breakpoints()
call("debug.resume")

# add_memory without access is a write breakpoint
bp = call("debug.breakpoints.add_memory", {"start": 7, "end": 7})["id"]
assert {b["id"]: b["type"] for b in call("debug.breakpoints.list")["breakpoints"]}[bp] == "mem_write"
clear_breakpoints()

# disassembly wraps at 64K instead of running off the end
wrapped = call("debug.disassemble", {"address": 0xFFFF, "count": 3})["instructions"]
for prev, nxt in zip(wrapped, wrapped[1:]):
    assert nxt["address"] == (prev["address"] + prev["length"]) & 0xFFFF, wrapped
assert all(0 <= i["address"] <= 0xFFFF for i in wrapped)

# render defaults are pal on and scanlines on: needs a non-black picture, so give the border a colour
load_program()
image = bytearray(base64.b64decode(call("snapshot.save", {"version": 1})["data_base64"]))
image[0x2F + 16] = 0x0B                                                  # border pen: a bright, non-black colour
image[0x43:0x43 + 18] = bytes([63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0, 0, 0, 0, 0])   # the standard 6845 setup, so a picture exists without any firmware
call("snapshot.load", {"data_base64": b64(bytes(image))})
call("run.frames", {"count": 8})
default = call("screen.render")["data_base64"]
assert set(base64.b64decode(default)) != {0, 255}, "the test needs a picture that is not black"
assert default == call("screen.render", {"pal": True, "scanlines": True})["data_base64"], "pal and scanlines default to true"
assert default != call("screen.render", {"pal": False, "scanlines": True})["data_base64"], "pal changes the picture"
assert default != call("screen.render", {"pal": True, "scanlines": False})["data_base64"], "scanlines changes the picture"

# ---- contract: api/schema.json and the code list exactly the same methods ----
import re
schema = json.loads((root / "api" / "schema.json").read_text())
assert schema["api_version"] == caps["api_version"], "schema and session.capabilities disagree on the version"
for method in schema["methods"]:
    reply = raw(method, {})
    assert reply.get("error", {}).get("message") != "method not found", f"{method} is in the schema but not implemented"
implemented = set()
for source in (root / "Sources" / "CPCApi").glob("*.c"):
    implemented |= set(re.findall(r'strcmp\(\s*m\s*,\s*"([a-z_]+(?:\.[a-z_]+)+)"', source.read_text()))
assert len(implemented) > 50, "the method scan found too little: has the dispatch style changed?"
assert implemented == set(schema["methods"]), f"schema and code differ: {sorted(implemented ^ set(schema['methods']))}"

# ---- unknown methods are still unknown ----
fails("debug.nope", {}, "method not found")
fails("tape.nope", {}, "method not found")

proc.stdin.close()
assert proc.wait(timeout=10) == 0
print("headless API 1.3: PASS (input, display, audio, tape, disc media, debugger)")
