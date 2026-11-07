#!/usr/bin/env python3
"""Five repeatable single-cold-run timings using the supplied GlasnostFWK fixture."""
import hashlib
import importlib.util
import os
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
fixture_dir = Path(os.environ.get("BLUEPIXEL_FIXTURE_DIR") or Path(__file__).resolve().parents[1] / "fixtures" / "nave")
if not os.environ.get("BLUEPIXEL_ROM_DIR"):
    raise SystemExit("BLUEPIXEL_ROM_DIR must point to the external ROM directory")
for line in (fixture_dir / "SHA256SUMS").read_text().splitlines():
    digest, filename = line.split(None, 1)
    filename = filename.lstrip("* ")
    if filename in {"nave.dsk", "nave.bin", "nave.noi", "fixture.json", "acceptance.json"}:
        actual = hashlib.sha256((fixture_dir / filename).read_bytes()).hexdigest()
        if actual != digest:
            raise SystemExit(f"SHA-256 mismatch for {filename}")

spec = importlib.util.spec_from_file_location(
    "acceptance_nave", ROOT / "Tests/headless/test_acceptance_nave.py"
)
acceptance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acceptance)
if not acceptance.BINARY.is_file():
    raise SystemExit("build/bluepixel-headless is missing; build it with tools/build-headless.sh first")

samples = []
hashes = []
for index in range(5):
    started = time.perf_counter()
    state_hash, frames, _ = acceptance.cold_run(fixture_dir)
    elapsed = time.perf_counter() - started
    factor = frames / elapsed / 50.08
    samples.append((elapsed, factor, frames))
    hashes.append(state_hash)
    print(f"run {index + 1}: frames={frames}, seconds={elapsed:.3f}, realtime={factor:.2f}x, hash={state_hash}", flush=True)

if len(set(hashes)) != 1:
    raise SystemExit(f"cold-run hashes differ: {hashes}")
median = statistics.median(sample[1] for sample in samples)
minimum = min(sample[1] for sample in samples)
maximum = max(sample[1] for sample in samples)
print(
    f"summary: repeats=5, median={median:.2f}x, min={minimum:.2f}x, max={maximum:.2f}x, "
    f"frames/run={samples[0][2]}, hash={hashes[0]}"
)
