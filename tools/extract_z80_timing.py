#!/usr/bin/env python3
"""Regenerate Sources/CPCCore/z80_timing.h from a WinAPE 2.0b2 WinApe.exe.
Usage: extract_z80_timing.py /path/to/WinApe.exe
The three 256-byte NOP-count tables (main, CB, ED) start at file offset 0x1cb8f0."""
import sys
d = open(sys.argv[1], 'rb').read()
b = 0x1cb8f0
assert d[b:b+8] == bytes([1, 3, 2, 2, 1, 1, 2, 1]), "unexpected WinApe.exe build"
def arr(name, off, n=256):
    rows = ['    ' + ', '.join(str(x) for x in d[off+i:off+i+16]) + ',' for i in range(0, n, 16)]
    return f"static const uint8_t {name}[{n}] = {{\n" + '\n'.join(rows) + "\n};\n"
out = ("/* Z80 instruction timings in CPC NOPs (1us). Recovered from WinAPE 2.0b2 (offset 0x1cb8f0).\n"
       "   Prefix fetches (CB/ED/DD/FD) add 1 on top of these. */\n#pragma once\n#include <stdint.h>\n\n")
out += arr("z80_nops_main", b) + arr("z80_nops_cb", b+256) + arr("z80_nops_ed", b+512)
open(sys.argv[2] if len(sys.argv) > 2 else 'Sources/CPCCore/z80_timing.h', 'w').write(out)
