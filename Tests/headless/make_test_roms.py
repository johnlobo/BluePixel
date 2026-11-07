#!/usr/bin/env python3
"""Generate disposable patterned ROM files for transport/API tests (not emulation tests)."""
import hashlib
import json
import sys
from pathlib import Path

directory = Path(sys.argv[1])
directory.mkdir(parents=True, exist_ok=True)
machine = {}
for role, seed in (("os", 17), ("basic", 73), ("amsdos", 131)):
    data = bytes(((i * 29 + seed) & 0xFF) for i in range(16384))
    filename = f"test-{role}.rom"
    (directory / filename).write_bytes(data)
    machine[role] = {
        "file": filename,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
(directory / "roms.json").write_text(
    json.dumps({"format": 1, "machines": {"cpc6128": machine}}, indent=2) + "\n"
)
