#!/usr/bin/env python3
"""Collect reproducible nave acceptance, five-run timing, and optional Callgrind data."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, help="new directory for binaries, logs and JSON reports")
parser.add_argument("--profile", action="store_true", help="also run one cold sequence under Callgrind")
parser.add_argument("--allow-other-host", action="store_true", help="allow a host other than Linux x86_64; results are labelled accordingly")
args = parser.parse_args()
target_host = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
if not target_host and not args.allow_other_host:
    parser.error("run this on the Linux x86_64 VPS, or use --allow-other-host for a labelled local check")
if args.profile and (not shutil.which("valgrind") or not shutil.which("callgrind_annotate")):
    parser.error("--profile needs valgrind and callgrind_annotate (Ubuntu: sudo apt-get install valgrind)")
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
output = (args.output or ROOT / "build" / "perf" / stamp).resolve()
output.mkdir(parents=True, exist_ok=False)
env = dict(os.environ)
env.setdefault("BLUEPIXEL_ROM_DIR", str(ROOT / "web" / "roms"))
env.setdefault("BLUEPIXEL_FIXTURE_DIR", str(ROOT / "Tests" / "fixtures" / "nave"))
env.setdefault("LDFLAGS", "-static" if platform.system() == "Linux" else "")
env["BLUEPIXEL_NO_BUILD"] = "1"
env["BLUEPIXEL_HEADLESS_BINARY"] = str(output / "bluepixel-headless")
# A profiler prefix is set only for the separate instrumented run below.
env.pop("BLUEPIXEL_HEADLESS_PREFIX", None)
if not (Path(env["BLUEPIXEL_ROM_DIR"]) / "roms.json").is_file():
    parser.error("the ROM directory must contain roms.json (default: web/roms)")

def capture(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()

metadata = {
    "utc": stamp, "system": platform.platform(), "architecture": platform.machine(),
    "linux_x86_64": target_host,
    "note": "Confirm this is the intended physical VPS; architecture alone does not detect CPU emulation or shared-vCPU load.",
    "commit": capture(["git", "rev-parse", "HEAD"]),
    "worktree_status": capture(["git", "status", "--porcelain"]),
    "compiler": capture(shlex.split(env.get("CC", "cc")) + ["--version"]),
    "cflags": env.get("CFLAGS", "-O2"), "ldflags": env["LDFLAGS"],
    "rom_dir": env["BLUEPIXEL_ROM_DIR"], "fixture_dir": env["BLUEPIXEL_FIXTURE_DIR"],
}
if Path("/proc/cpuinfo").is_file():
    metadata["cpuinfo"] = Path("/proc/cpuinfo").read_text()
(output / "host.json").write_text(json.dumps(metadata, indent=2) + "\n")

def run(command, logfile, child_env=env):
    print("+ " + shlex.join(map(str, command)), flush=True)
    with (output / logfile).open("w") as log:
        process = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=child_env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        if process.wait():
            raise SystemExit(f"command failed; see {output / logfile}")

run([ROOT / "tools/build-headless.sh", env["BLUEPIXEL_HEADLESS_BINARY"]], "build.log")
run([env["BLUEPIXEL_HEADLESS_BINARY"], "--version"], "version.log")
run(["python3", ROOT / "Tests/headless/test_acceptance_nave.py"], "acceptance.log")
run(["python3", ROOT / "Tests/headless/bench_nave.py", "--json", output / "benchmark.json"], "benchmark.log")
if args.profile:
    profile_env = dict(env)
    profile_env["BLUEPIXEL_HEADLESS_BINARY"] = str(output / "bluepixel-headless-profile")
    profile_env["CFLAGS"] = env.get("CFLAGS", "-O2") + " -g -fno-omit-frame-pointer"
    # Dynamic libc gives Callgrind useful library names. These timings are never used as the benchmark result.
    profile_env["LDFLAGS"] = ""
    run([ROOT / "tools/build-headless.sh", profile_env["BLUEPIXEL_HEADLESS_BINARY"]], "profile-build.log", profile_env)
    profile_env["BLUEPIXEL_HEADLESS_PREFIX"] = shlex.join([
        "valgrind", "--tool=callgrind", "--callgrind-out-file=" + str(output / "callgrind.out"),
    ])
    run(["python3", ROOT / "Tests/headless/bench_nave.py", "--runs", "1", "--json", output / "profile-run.json"], "profile.log", profile_env)
    run(["callgrind_annotate", "--auto=no", "--inclusive=no", "--threshold=100", output / "callgrind.out"], "callgrind-functions.txt")
print(f"Results: {output}")
print("Use benchmark.json for wall-clock speed; Callgrind counts identify hot paths, not real-time speed.")
