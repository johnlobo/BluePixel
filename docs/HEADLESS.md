# BluePixel headless H1

Build on the current host with `tools/build-headless.sh`. The executable reads one JSON-RPC 2.0 request per line from stdin and writes one response per line to stdout through the `CPCServer` stdio transport. It has no network socket in H1. Use `--version` to print its version.

## Probar en Linux

En Ubuntu 24.04 instala `build-essential` (gcc and libc headers), `zlib1g-dev` (including static `libz.a`) and `python3`. No cmake, Node.js or Emscripten is needed. Configure `BLUEPIXEL_ROM_DIR` to a directory with `roms.json` and the external ROM images; configure `BLUEPIXEL_FIXTURE_DIR` to the supplied `nave` fixture directory.

```sh
LDFLAGS="-static" tools/build-headless.sh
python3 Tests/headless/test_api.py
python3 Tests/headless/test_acceptance_nave.py
```

`build-headless.sh` reads `CC` (default `cc`), `CFLAGS` (default `-O2`) and `LDFLAGS` (empty by default). The executable accepts `--rom-dir PATH`; otherwise it checks `BLUEPIXEL_ROM_DIR`, `roms/` beside the executable, then `~/.config/bluepixel/roms`. The tests read `BLUEPIXEL_ROM_DIR` and `BLUEPIXEL_FIXTURE_DIR` and do not contain fixed local paths.

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

The web app still calls its older direct WASM bridge instead of `CPCApi`. H1 makes the API available to native builds; routing web through the same dispatcher remains necessary for the architecture's shared-semantics guarantee. `boot_ready` currently requires a configured address; AMSDOS header auto-detection is pending. The capture returns the last completed frame in canonical RGBA8 order; VRAM, palette, and CRTC metadata are not yet included.

`state_hash` has been checked for same-build save/restore and deterministic replay. Its current implementation serializes the C machine struct, so equivalence across different ABIs, especially wasm32 versus native, remains unverified. A canonical field-by-field serializer is still required before claiming cross-build hashes. The Linux static build and 50x performance target also remain unverified from this macOS checkout.
