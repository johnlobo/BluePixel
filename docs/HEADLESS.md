# BluePixel headless H1

Build on the current host with `tools/build-headless.sh`. The executable reads one JSON-RPC 2.0 request per line from stdin and writes one response per line to stdout through the `CPCServer` stdio transport. It has no network socket in H1. Use `--version` to print its version.

Input lines are limited to 1 MiB. Oversized lines receive a JSON-RPC invalid-request error and are drained before the next request.

## Probar en Linux

En Ubuntu 24.04 instala `build-essential` (gcc and libc headers), `zlib1g-dev` (including static `libz.a`) and `python3`. No cmake, Node.js or Emscripten is needed. Configure `BLUEPIXEL_ROM_DIR` to a directory with `roms.json` and the external ROM images; the `nave` fixture is in `Tests/fixtures/nave/` and is used by default; set `BLUEPIXEL_FIXTURE_DIR` only to test another copy.

```sh
LDFLAGS="-static" tools/build-headless.sh
python3 Tests/headless/test_api.py
python3 Tests/headless/test_acceptance_nave.py
```

`build-headless.sh` reads `CC` (default `cc`), `CFLAGS` (default `-O2`) and `LDFLAGS` (empty by default). The executable accepts `--rom-dir PATH`; otherwise it checks `BLUEPIXEL_ROM_DIR`, `roms/` beside the executable, then `~/.config/bluepixel/roms`. The tests read `BLUEPIXEL_ROM_DIR` (required; ROMs are never in the repository) and optionally `BLUEPIXEL_FIXTURE_DIR` (default `Tests/fixtures/nave/`), and do not contain fixed local paths.

Example:

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"session.capabilities","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"run.frames","params":{"count":50}}' \
  | ./build/bluepixel-headless
```

See [`api/schema.json`](../api/schema.json) for methods and data formats. Disk, CPC SNA snapshots, and exact machine state images are passed as `data_base64`, so the API dispatcher has no filesystem access. Use `snapshot.load` for a `.sna` file. Use `state.save` / `state.restore` for exact in-memory state round trips; that state format is build-specific. `screen.capture` returns raw 32-bit pixels in the core's `0xAARRGGBB` byte layout; clients must account for host byte order.

The dispatcher uses vendored [jsmn](https://github.com/zserge/jsmn), a small tokenizing JSON parser. Its MIT license is included in the header and [LICENSES/jsmn-MIT.txt](../LICENSES/jsmn-MIT.txt). It needs no allocation-heavy object model or runtime library dependency and can be compiled into both native and WASM targets.

File-based DSK and symbol helpers now live in `CPCHost`; `CPCCore` parses in-memory symbols and disk data only. Optional I/O tracing uses a host callback instead of writing to stderr from the core.

ROM images are not included in the repository or executable. The ROM directory contains a `roms.json` manifest with `machines.cpc6128.{os,basic,amsdos}`, each entry providing a basename, 16384-byte size and SHA-256. See [`roms.example.json`](../roms.example.json) and [`api/roms.schema.json`](../api/roms.schema.json). The headless loader rejects missing, wrong-sized or hash-mismatched ROMs before accepting commands. The API also accepts ROM bytes through `rom.load`.

The H1 `state.hash` combines the serialized emulated state with hashes of mounted disk contents and the bytes of loaded ROMs. The exact state image remains build-specific and expects the same media and ROM configuration when restored.

The clock is virtual. Only `run.*` advances emulation. One NOP is four Z80 T-states; `run.nops` advances by whole instructions, so it can exceed the requested NOP count. `t_states_aligned` reports NOP-aligned counts. `run.frames` advances to successive CRTC flybacks.

`boot_ready.configure` currently requires an explicit entry address. It reports ready when the PC reaches that address and the core's text auto-type queue is drained. Automatic extraction of the AMSDOS program entry point from an inserted disk is not implemented in H1.

Run the H1 API smoke test with `python3 Tests/headless/test_api.py`. The Linux static build is intended to use a Linux x86_64 toolchain with static zlib; the build script accepts `CC` and `LDFLAGS` overrides. A successful macOS build does not establish Linux static compatibility.

On the available macOS arm64 host, after a 300-frame warm-up, a 1,000-frame BASIC idle batch took 0.428 seconds (19.968 virtual seconds, 46.6x). This was not the GlasnostFWK fixture and does not verify the Linux VPS target of 50x; the Linux static workflow has not run from this checkout.

The supplied `nave` fixture hashes matched `SHA256SUMS`. The full acceptance script loads the DSK and symbols, resets the CPC6128, waits for configured entry `0x0B72`, runs `RUN"NAVE"`, checks the game status and movement limits, captures the display, and compares cold-start and restored-state replays. On the available macOS arm64 host it passed with hash `a8750fbe248c354e`, equal cold-start hashes, and equal restore replay. It ran 2,740 virtual frames in 4.687 seconds (11.7x real time); `run.frames` accounted for 4.628 seconds, while input, memory, hash, state, capture and transport took 0.060 seconds. This is below the 50x Linux VPS target and is not a Linux measurement.

The web app still calls its older direct WASM bridge instead of `CPCApi`. H1 makes the API available to native builds; routing web through the same dispatcher remains necessary for the architecture's shared-semantics guarantee. `boot_ready` currently requires a configured address; AMSDOS header auto-detection is pending. `screen.capture` returns the last completed frame in RGBA8 order. Passing `{"metadata":true}` additionally returns the screen mode, CRTC type and registers, hardware palette indices, and current ARGB palette.

`state_hash` has been checked for same-build save/restore and deterministic replay. Its current implementation serializes the C machine struct, so equivalence across different ABIs, especially wasm32 versus native, remains unverified. A canonical field-by-field serializer is still required before claiming cross-build hashes. H2 results and the Linux profiling method are recorded below.

## H2

### 1. Velocidad

**PARCIAL.** Profile first, then optimize only measured hot paths. On the H1 Linux VPS described in the supplied report, `-O2` took 5.1–5.6 s for 2,740 frames (9.8–10.6x); `-O3 -march=native` took 4.5–4.8 s (11.5–12.3x). These are third-party reference measurements, not measurements from this checkout.

This Mac has no physical x86_64 Linux host. I used Ubuntu 24.04 x86_64/GCC 13.3.0 under QEMU on an Apple Silicon Mac, 4 guest CPUs. This allowed Linux builds and profiling but emulates the x86 CPU, so its wall times cannot stand in for the Haswell VPS. `perf` was unavailable for the VM's running kernel; gprof (`-O2 -pg`) was used instead. Each profile is one 1,370-frame cold acceptance run. The detailed top-ten tables are in [`docs/perf/h2-profile.md`](perf/h2-profile.md).

The profile showed per-instruction debugger checks, per-T-state idle tape/FDC work, and then the Z80, video and PSG paths. The core changes skip code-breakpoint checks when the debugger is inactive, skip null tape ticks, and batch only idle FDC spans; active FDC/seek spans still tick one time unit at a time. The first attempt batched active FDC spans and changed the hash; that version was reverted. The final version preserves the H1 hash.

The five-run `bench_nave.py` comparison in this QEMU VM was:

| Build | Median real time | Min | Max | Hash |
|---|---:|---:|---:|---|
| H1 baseline, `-O2` | 1.73x | 1.70x | 1.80x | `a8750fbe248c354e` |
| H2, default `-O2`, static | 1.90x | 1.73x | 1.95x | `a8750fbe248c354e` |

The H2 median is about 9.8% higher in this emulated environment, but still far below 50x and not a VPS result. The acceptance command passed with 2,740 frames, equal cold hashes and equal restore replay; its QEMU wall time was 29.947 s (1.8x). The function profile still puts `cpc_step`, `cpc_video_tick`, `psg_tick` and `z80_step` at the top. The remaining gap needs a deeper timing/video/audio redesign and measurement on physical Linux x86_64.

### 2. Estado canónico

**NO HECHO.** State save/hash still use the H1 build-specific C-struct image. The H1 hash is `a8750fbe248c354e`; the H2 performance changes preserve it. No new canonical-format hash was produced, and cross-ABI equivalence is not claimed. Completing this requires replacing the struct image and nested device snapshots with one versioned field-by-field little-endian serializer used by hash, save, restore and handoff.

### 3. Web sobre CPCApi

**NO HECHO.** `web/app.js` still calls `web/bridge.c` exports directly. I did not claim native/WASM conformance: zero of the 10 requested 100-frame hash checkpoints were compared, and normal browser behavior was not re-verified in H2. The remaining work is to expose the same dispatcher to WASM, route the UI through it, remove the old bridge, and run the requested Node conformance sequence.

### 4. Pendientes H1

- **Matriz de teclado: HECHO en H1; confirmado.** `test_acceptance_nave.py` drives `input.matrix` with active-low row/column masks, and the actual game reaches all four movement limits.
- **Metadata de captura: HECHO.** `screen.capture` remains unchanged by default. With `metadata: true`, its response also includes `screen_mode`, `crtc_type`, 18 CRTC registers, 17 hardware palette indices and 17 current ARGB colours. The API smoke checks these fields.
- **Autodetección `boot_ready`: NO HECHO.** It still requires an explicit address; extracting the AMSDOS execution address from a file on a mounted DSK remains pending.

The Linux commands run in the QEMU x86_64 guest were:

```text
LDFLAGS="-static" tools/build-headless.sh         Built bluepixel-headless
./build/bluepixel-headless --version               bluepixel-headless 0.1.0-h1
python3 Tests/headless/test_api.py                 headless API smoke: PASS
python3 Tests/headless/test_acceptance_nave.py     PASS nave: hash=a8750fbe248c354e, cold_hash_equal=yes, restore_replay=yes
python3 Tests/headless/bench_nave.py               repeats=5, median=1.90x, min=1.73x, max=1.95x
```

ROMs stayed outside the repository and executable. The QEMU environment verified x86_64 Linux compilation, API behavior, fixture behavior and the hash invariant; it does not verify the physical Haswell performance target. The native/WASM checkpoint test and canonical state equivalence also remain unverified.
