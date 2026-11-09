#!/usr/bin/env python3
import base64
import json
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
binary = root / "build" / "bluepixel-headless"
rom_dir = Path(os.environ["BLUEPIXEL_ROM_DIR"])
rom_manifest = json.loads((rom_dir / "roms.json").read_text())["machines"]["cpc6128"]
subprocess.run([str(root / "tools" / "build-headless.sh")], check=True, cwd=root)
proc = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

def call(i, method, params=None):
    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params or {}}) + "\n")
    proc.stdin.flush()
    result = json.loads(proc.stdout.readline())
    assert result["id"] == i, result
    assert "error" not in result, result
    return result["result"]

caps = call(1, "session.capabilities")
assert caps["api_version"] == "1.2" and caps["state_format"] == 1 and "headless" in caps["capabilities"]
call(13, "machine.reset")
call(27, "boot_ready.configure", {"address": 0})
assert call(28, "boot_ready.status")["ready"]
call(29, "input.matrix", {"row": 0, "mask": "0xFD", "pressed": True})
assert not call(30, "boot_ready.status")["keyboard_drained"]
call(31, "input.matrix", {"row": 0, "mask": "0xFD", "pressed": False})
assert call(32, "boot_ready.status")["keyboard_drained"]
assert "pc" in call(14, "cpu.registers")
assert call(15, "run.instructions", {"count": 2})["executed_instructions"] == 2
assert call(16, "run.nops", {"count": 7})["advanced_nops"] >= 7
call(17, "run.reset_tstates")
call(2, "mem.write", {"address": 0x4000, "value": 0xA5})
assert call(3, "mem.read", {"address": 0x4000, "length": 1})["hex"] == "a5"
symbols = base64.b64encode(b"DEF test_symbol 0x4000\n").decode()
assert call(33, "symbols.load", {"data_base64": symbols})["loaded"] == 1
assert call(34, "mem.read", {"address": "test_symbol", "length": 1})["hex"] == "a5"
basic_path = rom_dir / rom_manifest["basic"]["file"]
call(35, "rom.load", {"role": "basic", "data_base64": base64.b64encode(basic_path.read_bytes()).decode()})
blank_dsk = bytearray(256 + 0x1300)
blank_dsk[:34] = b"MV - CPCEMU Disk-File\r\nDisk-Info\r\n"
blank_dsk[0x30] = 1
blank_dsk[0x31] = 1
blank_dsk[0x32:0x34] = (0x1300).to_bytes(2, "little")
blank_dsk[0x100:0x10C] = b"Track-Info\r\n"
blank_dsk[0x114] = 2
blank_dsk[0x115] = 9
blank_dsk[0x116] = 0x4E
blank_dsk[0x117] = 0xE5
for sector in range(9):
    descriptor = 0x118 + sector * 8
    blank_dsk[descriptor:descriptor + 4] = bytes([0, 0, 0xC1 + sector, 2])
    data = 0x200 + sector * 512
    blank_dsk[data:data + 512] = bytes([0xE5]) * 512
call(18, "disc.load", {"drive": 0, "data_base64": base64.b64encode(blank_dsk).decode()})
media_hash = call(24, "state.hash")["hash"]
changed_dsk = bytearray(blank_dsk)
changed_dsk[0x200] = 0xE4
call(25, "disc.load", {"drive": 1, "data_base64": base64.b64encode(changed_dsk).decode()})
assert call(26, "state.hash")["hash"] != media_hash
call(19, "input.text", {"text": "A"})
assert call(20, "boot_ready.configure", {"address": 0})["configured"]
hash_before = call(4, "state.hash")["hash"]
snapshot = call(5, "state.save")["data_base64"]
call(6, "run.frames", {"count": 3})
hash_after = call(10, "state.hash")["hash"]
call(7, "state.restore", {"data_base64": snapshot})
assert call(8, "state.hash")["hash"] == hash_before
call(11, "run.frames", {"count": 3})
assert call(12, "state.hash")["hash"] == hash_after
amsdos_snapshot = call(21, "snapshot.save")["data_base64"]
call(22, "snapshot.load", {"data_base64": amsdos_snapshot})
assert "ready" in call(23, "boot_ready.status")
assert len(base64.b64decode(call(9, "screen.capture")["data_base64"])) == 768 * 270 * 4
capture_meta = call(36, "screen.capture", {"metadata": True})
assert capture_meta["metadata"]["screen_mode"] in range(4)
assert len(capture_meta["metadata"]["crtc_registers"]) == 18
assert len(capture_meta["metadata"]["palette_hw"]) == 17
assert len(capture_meta["metadata"]["palette_argb"]) == 17
proc.stdin.close()
assert proc.wait(timeout=10) == 0
print("headless API smoke: PASS (capabilities, memory, exact state/hash roundtrip, deterministic replay, capture)")
