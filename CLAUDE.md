# BluePixel

macOS port of the WinAPE Amstrad CPC/Plus emulator (WinAPE 2.0 beta 2). The Windows binary was analysed with
REA/Ghidra to recover timing tables, formats and behaviour; the emulator itself is written from scratch.
The owner writes Spanish: answer in Spanish, keep code, comments, UI strings and commit messages in English.
[docs/FEATURES.md](docs/FEATURES.md) is the feature inventory taken from WinAPE (what is and is not ported).

User manual (Spanish, HTML, start at [docs/index.html](docs/index.html)): living documentation — when a user-visible feature is added or changed, update the matching page (`debugger.html`, `sessions.html`, `command-line.html`, shortcuts table in `index.html`).

## Layout

- `Sources/CPCCore/` — the emulator, plain C, **no Apple dependencies** (keep it portable for WebAssembly/Linux).
  Z80, gate array, CRTC, PPI, PSG, FDC/DSK, tape, SNA, Plus ASIC, Multiface II, AMX mouse,
  disassembler, debugger core (`debug.c`, `debugview.c`), exact machine state (`state.c`) and session
  recording/replay (`replay.c`). Public headers in `include/`.
- `Sources/BluePixel/` — AppKit app (Swift). `Emulator.swift` owns the machine and its thread; `AppDelegate.swift`
  builds the menus; `Debug*.swift`, `DebuggerController.swift`, `SymbolsWindow.swift`, `HistoryWindow.swift`,
  `WatchLogWindows.swift` are the debugger UI; `SettingsController.swift`, `Pokes.swift`, `Help.swift`,
  `LaunchOptions.swift` (command line), `Session.swift` (record/replay menu actions) are the rest.
- `Sources/CPCApi/` — JSON-RPC 2.0 dispatcher in C (vendored `jsmn.h`); schema in `api/schema.json` (API 1.2, the contract: a test fails if the schema and
  the code list different methods). `api.c` core/state/run methods, `api_media.c` input/display/audio/tape/disc/ROM unload, `api_debug.c` `debug.*`,
  `api_util.c` + `api_internal.h` parsing and JSON helpers. A new method goes in the matching module, the schema and `Tests/headless/test_api_ext.py`.
  `Sources/CPCServer/` — stdio, authenticated Unix sockets and loopback WebSocket with token/Origin checks; see `docs/CONTROL-SERVER.md`. Desktop requests are queued to the emulator thread and use a borrowed CPCApi session.
  `Sources/CPCHost/` — file, ROM-manifest (`roms.json`) and SHA-256 helpers, so `CPCCore` and `CPCApi` do no file I/O.
- `Sources/bluepixel-headless/` — Linux/macOS executable: one JSON-RPC request per line on stdin, one response per line on
  stdout. Guide, protocol and H2 status: [docs/HEADLESS.md](docs/HEADLESS.md).
- `Sources/cpcrun/` — test runner and diagnostics for the core (no UI).
- `web/` — browser frontend using the portable C emulator through CPCApi JSON-RPC. Build with `tools/build-web.sh`;
  see `web/README.md` and `web/capi-rpc-client.js`.
- `tools/` — `bundle.sh` (builds `build/BluePixel.app` with icon), `bluepixel` (runs the bundle from a terminal),
  `build-headless.sh` (plain `cc` build of the headless executable, no Swift), `fetch_z80_tests.sh`,
  `extract_z80_timing.py`. `Resources/` holds the app icon.
- `Tests/headless/` — Python and C tests that run on Linux (`Tests/wasm/conformance.mjs` is the native-vs-WASM Node harness); `Tests/fixtures/nave/` is the GlasnostFWK acceptance fixture
  (DSK, symbols, `roms.json`, reference hash in `acceptance.json`).

## Build and run

```bash
swift build -c release            # both products
tools/bundle.sh                   # build/BluePixel.app (needed to get the icon / Info.plist)
tools/bluepixel game.dsk /A /SYM obj/game.noi
```

`build/BluePixel.app` is a bundle (a folder), not an executable: launch it with `open`, `tools/bluepixel` or
`build/BluePixel.app/Contents/MacOS/BluePixel`.

The browser build needs Emscripten and a local HTTP server; see [web/README.md](web/README.md).

Only `CPCCore`, `CPCApi`, `CPCServer`, `CPCHost` and `bluepixel-headless` build on Linux (the VPS has no Swift or AppKit):

```bash
LDFLAGS="-static" tools/build-headless.sh     # -> build/bluepixel-headless; CC, CFLAGS, LDFLAGS overridable
./build/bluepixel-headless --version
```

## Tests

There is no XCTest suite. The core tests are `cpcrun` commands, and one script runs all of them on Linux or macOS using only
repository material (ROMs from `web/roms`, the nave fixture disc, exercisers fetched and hash-checked into `tests/z80/`):

```bash
tools/run-core-tests.sh                # builds build/core/cpcrun with cc (needs -lz), ~2 min, progress on stderr, exit 1 on any failure
tools/run-core-tests.sh --skip-zexall  # or --only statetest ...; logs in build/core/logs/
```

It fails on a non-zero exit, any `FAIL` line, any `missing <rom>` message, any exerciser `ERROR`. CI: `.github/workflows/core.yml`
(PRs and pushes to `main`). Disc tests take `cpcrun <test> <roms> <disc> [game]`; `game` is `boop` (default) or `nave`
(the fixture, used by the script); a per-game table in `Sources/cpcrun/main.c` holds the program-specific parts (run command,
scripted keys, per-frame routine, RAM range). By hand, with a ROM folder that has the WinAPE names (`web/roms` does):

```bash
cc -std=gnu11 -O2 -ISources/CPCCore/include -ISources/CPCApi/include -ISources/CPCHost/include Sources/CPCCore/*.c Sources/CPCHost/host_files.c Sources/cpcrun/main.c -lz -o build/core/cpcrun
build/core/cpcrun zex tests/z80/zexdoc.com                     # ~2 min; must report no "ERROR"; also zexall.com
for t in dbgtest mftest amxtest; do build/core/cpcrun $t web/roms; done
build/core/cpcrun steptest web/roms Tests/fixtures/nave/nave.dsk nave   # also logtest
build/core/cpcrun symtest <symbols file> <symbol name>
```

`disctest` (a BASIC program writes a file during a session; replay/seek/clone must reproduce state and disc), `sotest` (reverse Step Over + markers), `tracetest` (break on change, access map, profiler + speed cost) and
`statetest` (save/restore at 9 points incl. mid-frame, 200 frames each) and `rectest` (record the game with every event
type, replay in another machine, 80 random seeks, injected input must be detected, edit cuts the recording) are the
exactness tests for the savestate/replay work: run them after touching anything in `CPCCore` that holds state.
They load ROMs by WinAPE file names (`OS6128.ROM`, `BASIC1-1.ROM`, `AMSDOS.ROM`); a folder with other names prints `missing ...` and runs without firmware. Also run
`python3 Tests/headless/check_state_coverage.py` (every field reaches the canonical state, restore fuzz under ASan;
needs gdb) and `python3 Tests/headless/test_state_abi.py` (same bytes from a normal and a `-fpack-struct` build).

### Headless tests (Linux or macOS)

ROMs are never committed. `BLUEPIXEL_ROM_DIR` must point at a directory with a valid `roms.json` (`roms.example.json`,
`api/roms.schema.json`); the loader rejects missing or hash-mismatched ROMs. Lookup order: `--rom-dir`, `BLUEPIXEL_ROM_DIR`,
`roms/` beside the executable, `~/.config/bluepixel/roms`. Each test is a standalone script, so a single test is just that script:

```bash
export BLUEPIXEL_ROM_DIR=/path/to/roms
python3 Tests/headless/test_api.py              # API smoke
python3 Tests/headless/test_render_golden.py       # video and sound exactness against hashes recorded from e4be45c (run after ANY change to CPCCore timing, video, PSG, tape, FDC)
python3 Tests/headless/fuzz_equivalence.py --reference <old bluepixel-headless> --seeds 300   # random machines, reference build vs current
python3 Tests/headless/test_api_ext.py          # API 1.2: input, display, audio, tape, discs, debugger, schema contract (BLUEPIXEL_SANITIZE=1: ASan/UBSan build)
node Tests/wasm/conformance.mjs --self-test     # conformance harness, native vs a second native process; --wasm <js> for the real comparison
python3 Tests/headless/test_acceptance_nave.py  # rebuilds first (BLUEPIXEL_NO_BUILD=1 skips); checks the fixture's reference hash
python3 Tests/headless/bench_nave.py            # speed; never quote a number you did not measure
SANITIZE=0 python3 Tests/headless/check_state_coverage.py   # SANITIZE=0 when libasan is missing
```

`BLUEPIXEL_EXPECT_HASH` overrides the acceptance reference hash (empty disables the check); the hash only holds for the ROMs pinned
in the fixture's `roms.json`. CI (`.github/workflows/headless.yml`, on PRs and pushes to `main`, `h1-headless`, `h2-core`) builds the
static Linux binary with disposable ROMs from `Tests/headless/make_test_roms.py <dir>` and runs `test_api.py`, `test_api_ext.py` (plain and sanitized),
`check_state_coverage.py`, `test_state_abi.py` and the conformance harness self-test. It also runs real-ROM nave acceptance and five timings with `tools/measure-headless.py`; the WASM job exercises the web adapter and native/WASM conformance. `deploy-pages.yml` publishes `web/` to GitHub Pages.

Other `cpcrun` commands: `boot` (renders a frame to BMP; options `--disc --type --wait --space --return --frames
--wav --out`; env `VIEW=raster|vram`, `MIDFRAME=<cycles>`, `DUMP=1`, `IOLOG=1 IOFROM=<frame>`), `dis`, `inttest`,
`tapetest`, `snaptest`, `newdisc`. `~/Proyectos/boop` (a CPCtelera game, its `.noi`/`.map` in `obj/`) is the usual
real-world test case.

UI checks run the app with hidden test flags, then read the PNG: `--panel <selector:> --panelat <s> --shotwin "<window title>"
--shot out.png --after <s> --type "text~"` (`~` = Return). `bitmapImageRepForCachingDisplay` renders some AppKit
controls blank (tab strips, segmented controls); use `screencapture` when the real look matters.

## Where things come from

- ROMs: `~/Documents/WinAPE20B2/ROM` (override with `[BluePixel] ROM Folder`). Per-user data lives in
  `~/Library/Application Support/BluePixel/` (`BluePixel.ini`, `Profile/`, `Help/`, `WINAPE.POK`).
- Help: pages extracted from `WinAPE.chm` with `extract_chmLib` (brew `chmlib`) into `Help/`; the viewer is `Help.swift`.
- WinApe.exe addresses worth knowing (Ghidra, image base 0x400000): AMX mouse per-frame logic 0x5989FA,
  palette intensity tables 0x5CCA14 (linear) / 0x5CCA44 (non-linear), INI key table 0x5CBCA4.
- `WINAPE.POK` format is decoded in `Pokes.swift` (header comment); it round-trips byte for byte.
- Debugger window layout and shortcuts mirror WinAPE; the guide is in the owner's Docs ("BluePixel — Guía del depurador").

## Hard-won facts (do not re-derive)

- **Mode 0 pixels:** pen bits are b7,b3,b5,b1 (pixel 0) and b6,b2,b4,b0 (pixel 1). Applies to `cpc.c`, `debugview.c`
  and the Graphics Finder.
- **PSG:** the AY follows PPI port A while BDIR/BC1 are held (latch address, then data changes re-latch). Arkos AKG
  depends on it; `psg_ctrl_update` is also called when port A is written.
- **Palette:** 3 gate-array levels are intensities 0/6/15 mapped through the 16-entry tables above; Plus uses 4-bit
  per channel through the same tables. Linear Palette defaults to **off** (CPC half-tones are #80).
- **Audio is the master clock:** the emulation thread waits on the audio queue (~3 frames), not on `usleep`.
  Underruns hold the last sample. Do not "fix" timing by sleeping.
- **Debugger stepping:** `dbg_step_into` completes a `HALT`; the emulation loop re-checks `paused` under the lock;
  `debugPause`/`debugResume` cancel pending `temp_bp`/`step_out`. Step Out = run until a RET leaves the frame
  (`step_out_sp`). Every stop re-focuses the disassembler on PC.
- **Debugger tools (`dbgtrace.c`, `TraceWindows.swift`):** break on change (`BP_CHANGE`, evaluated before each instruction: registers
  and `peek/word(const)` read directly, anything else through `dbg_eval`), memory access map (needs `watch[]` all set; the Z80 has
  a separate `fetch` callback so opcode bytes count as executed, not read; `cpc_peek` is event-free, use it for host/debugger reads),
  and a profiler with a shadow call stack (CALL/RST/interrupt push, RET pops; self vs inclusive per entry address). All stay quiet
  while `Debugger.quiet`/`scanning` (seeks, reverse re-simulation). A C struct field called `self` is unusable from Swift.
- **Expressions:** parentheses only group; read memory with `peek(addr)`. Comparisons are `=` or `==`, `<>`
  (not `!=`), `<`, `>`, `<=`, `>=`; logic is `and`, `or`, `xor` (not `&&`/`||`); true is -1. Logpoint text uses
  `{expr}` (hex) / `{expr:d}`.
- **Symbols:** `.noi` (`DEF name 0xADDR`), SDCC `.map` (8-hex-digit address column), RASM, sjasmplus and Maxam lists.
  `s__*`, `l__*` and `.__.` linker symbols are skipped. A `.dsk` and `.noi` from different builds silently mislabel code.
- **CPC_Symbols.ttf:** its format-0 cmap is ignored by CoreText and `CTFontDrawGlyphs` draws nothing; glyph ids are read
  from the file and outlines are filled as paths (`DebugStyle.drawGlyphs`). The font is WinAPE's: don't redistribute it.
- **macOS keys:** F-keys need fn unless the system setting is changed; Ctrl+F8 is taken by the system, so scanline
  stepping is ⌥F8. Pause (F7) and Step Into share F7 by design: F7 pauses when running and steps when paused.
- **Savestates are exact, SNA restores are not** (1-2 µs phase and RAM differences): replay needs `state.c`. The
  state is a canonical stream (format 1, `ser.h`): one visit function per part (`visit_*` in `state.c`,
  `fdc_state_visit`, `tape_state_visit`, `cpc_autotype_state_visit`) serializes each field little endian, and the
  same function writes, hashes, validates and restores. Equal machines give equal bytes on every build/ABI; state
  files and `.cpcrec` recordings load in any build. **A new field in `struct CPC`, `struct FDC` or `struct CPCTape`
  must be added to its visit function, or to `NOT_STATE` (with the reason) in `Tests/headless/check_state_coverage.py`**,
  which reads the layout from the debug info and fails on any unlisted byte that does not reach the stream. Values
  that decide the shape of the stream (lengths, counts, presence) go through `ser_count`/`ser_flag`, because CHECK
  mode never writes fields; indices the core uses into arrays go through `ser_int_in`/`ser_u8_in`, relations
  between fields are checked on a scratch copy (`fdc_consistent`, `tape_consistent`): states come from API
  clients, and `check_state_coverage.py` restores hostile values and drives the machine under ASan/UBSan.
  `ser_int` rejects |v| > 2^24 and floats must be finite. The auto-type queue is `CPC.autotype` (freed by `cpc_destroy`). `state.hash` = FNV-1a 64 of the stream + disc CRCs + ROM bytes; nave reference `be44e742783fb94c`
  (H1 struct-image format gave `a8750fbe248c354e`).
- **Disc writes in sessions:** the first FDC write to a track keeps a copy (`CPCDisc.orig`, `dsk_touch`); a state embeds
  the tracks that have one (geometry too), restoring puts tracks back from `orig` if the state lacks them. Recording
  start clears `orig`; stop/taint stores the originals as pre-images in the file; `cpc_player_begin` applies them first
  (so the disc ends up as at record start, whatever happened to it since) and `cpc_recording_disc_matches` compares by
  `dsk_content_crc`. A replay must never commit the user's image (`commitDisc` skips while `replayActive`).
- **Replay determinism rules:** host inputs go through `replay_input` (logged with the emulated µs, ignored while a
  replay runs); the `_raw` variants bypass it. Per-frame peripherals (auto-type, AMX) tick at the frame flyback, not
  on entry to `cpc_run_frame`, because the debugger slices execution. Keyframes/hashes are taken in
  `replay_frame_done` (instruction boundary after the flyback), never inside `advance()`. Seeking detaches the
  debugger. Anything that changes the machine otherwise (media, settings, register/memory edits) must call
  `noteUnreproducible` / `cpc_record_taint`, which ends the recording there. Recording files (version 4) are
  little endian and hold a canonical state, so any build loads them; versions 1-3 are rejected.
- **Transport bar design:** forward play/pause use the normal emulation thread. Reverse play never runs the machine
  backwards: `cpc_player_capture` re-simulates one 50-frame interval from its keyframe and the pictures are shown
  in reverse; stopping reverse does an exact seek. A clone of the machine (`cpc_clone_machine`, discs deep-copied)
  indexes the whole recording on a background thread and hands its keyframes over with
  `cpc_player_adopt_keyframes` (machine lock held); never call `stopIndexing()` with that lock held.
- **Reverse debugging rules:** a position is `c->cycles` (emulated µs, unique per step). Step back = restore the last
  keyframe strictly before, re-simulate to find where the last step began, then `cpc_player_goto_cycles` (which also
  refills the execution history). Reverse searches (`cpc_reverse_*`) stop *before* the instruction responsible, ignore
  pass counts and logpoints (`Debugger.scanning`) and restore the user's breakpoint table afterwards. While a search
  runs on its worker thread the live machine is borrowed: UI actions that touch it must check `emu.isSearching`.
  `frame_ready` is not state (it differed between run-driven and step-driven positions).
- Frame counters can go backwards (restore/seek): never subtract them unsigned (this crashed the status bar once).
- `DbgView` has `lower_base`/`cart_bank` (Plus secondary mapping); `Debugger.watch[]` is too large for Swift to import,
  use `dbg_watch_bits()`.

## Conventions

- C99 in the core, 4-space indent, short comments that say *why*. Swift follows the surrounding style (dense,
  one-line handlers are normal here). Keep `CPCCore` free of AppKit/Foundation.
- Formatting: `Sources/CPCApi/api.c`, `api_util.c`, `Sources/CPCServer/network.c`, `stdio.c` and `Sources/bluepixel-headless/main.c` are
  formatted with clang-format 23.1.5 (`.clang-format`: 4 spaces, 120 columns, braces always); run it on those files after editing. `Sources/CPCCore`
  keeps its dense style (short one-line handlers) and is never reformatted; neither is the vendored `jsmn.h`.
- Warnings are errors in CI: `tools/lint.sh` (`.github/workflows/lint.yml`) builds every portable C source with `-Wall -Wextra -Wshadow -Werror`
  at `-O0` and `-O2` and runs `clang-format --dry-run --Werror` on the formatted files. Fix the cause (rename, braces, cast); do not add pragmas or `-Wno-`.
- Temporary test harnesses in the app (env-var hooks) must be removed before committing: `grep -n "DBG[A-Z0-9]*TEST\|DBGB[0-9]"`.
- Commit messages: imperative summary, then a short paragraph of what and why.
- `.dsk` files and WinAPE's own files (`WinApe.exe`, `CPC_Symbols.ttf`, the CHM) are not in the repo. CPC ROMs for the browser build are in `web/roms/`.

## Not done yet

Headless (status table in `docs/HEADLESS.md`): the 50x realtime target is not met (about 21x on the 4 vCPU VPS after the exact optimizations in `docs/perf/h2-profile.md`; 50x was analysed on 2026-11-12 and judged feasible but not worth it yet: it needs an event-driven PSG and CRTC, a lazy span painter, a debugger-free run loop and a flat-switch Z80 together, see "Análisis de viabilidad de 50x" there; `perf` is blocked
in the container, use callgrind); CPCApi 1.4 and the browser JSON-RPC migration are complete, with native/WASM state conformance
passing locally. Chrome game loading, stepping and breakpoints are verified; the WASM adapter has a functional test in CI; desktop control and Unix/WebSocket transports are implemented (off by default), with shutdown cancellation and owner-thread dispatch;
`boot_ready.configure` takes an explicit `address` or a `file` whose AMSDOS header gives the entry address (API 1.4); Linux/macOS/WASM trace comparison and all-direction state handoff are in `.github/workflows/portable.yml` / `Tests/wasm/portable.mjs`; GlasnostFWK has a stdio adapter and its own freshly built nave CI smoke (see steps 18/19 in `docs/HEADLESS.md`); check the latest runs. For VPS performance work, follow `docs/HANDOFF-VPS-H2.md` and `tools/measure-headless.py`.

Session record/playback is complete: core, File menu, transport bar (`Transport.swift`), reverse debugging (`ReverseDebug.swift`: Step Back
⌥F7, Step Back Over ⌥⌘F8, Reverse Continue ⌥F9, Run Back to Cursor ⌥F4, "Who Wrote/Read This?"), disc writes and markers (`CPC_MARK_TEXT`,
stored in the `.cpcrec` and saved back when edited). Integrated Maxam-compatible assembler
(menu item says "Not available yet"), disc/sector editor, Format Disc, Flip Disc, AVI/WAV/YM recording, Symbiface II (IDE/RTC/PS2 mouse), 4 MB Yarek RAM, printer, ZIP/RAR,
tape CSV, DSC/HDR discs, gamepad joystick. Debugger ideas not built: DeZog/VS Code protocol, AY channel viewer.
The browser port is in `web/`: CPC 6128, bundled ROMs with automatic CPC 6128 startup, DSK, SNA, tape, keyboard, audio, disk export and debugger. Plus
configuration and session tools are not available in the browser UI.
