# MacAPE

macOS port of the WinAPE Amstrad CPC/Plus emulator (WinAPE 2.0 beta 2). The Windows binary was analysed with
REA/Ghidra to recover timing tables, formats and behaviour; the emulator itself is written from scratch.
The owner writes Spanish: answer in Spanish, keep code, comments, UI strings and commit messages in English.
[docs/FEATURES.md](docs/FEATURES.md) is the feature inventory taken from WinAPE (what is and is not ported).

User manual (Spanish, HTML, start at [docs/index.html](docs/index.html)): living documentation — when a user-visible feature is added or changed, update the matching page (`debugger.html`, `sessions.html`, `command-line.html`, shortcuts table in `index.html`).

## Layout

- `Sources/CPCCore/` — the emulator, plain C, **no Apple dependencies** (keep it portable: a WebAssembly/Linux build
  is a possible future). Z80, gate array, CRTC, PPI, PSG, FDC/DSK, tape, SNA, Plus ASIC, Multiface II, AMX mouse,
  disassembler, debugger core (`debug.c`, `debugview.c`), exact machine state (`state.c`) and session
  recording/replay (`replay.c`). Public headers in `include/`.
- `Sources/MacAPE/` — AppKit app (Swift). `Emulator.swift` owns the machine and its thread; `AppDelegate.swift`
  builds the menus; `Debug*.swift`, `DebuggerController.swift`, `SymbolsWindow.swift`, `HistoryWindow.swift`,
  `WatchLogWindows.swift` are the debugger UI; `SettingsController.swift`, `Pokes.swift`, `Help.swift`,
  `LaunchOptions.swift` (command line), `Session.swift` (record/replay menu actions) are the rest.
- `Sources/cpcrun/` — headless test runner and diagnostics for the core (no UI).
- `tools/` — `bundle.sh` (builds `build/MacAPE.app` with icon), `macape` (runs the bundle from a terminal),
  `fetch_z80_tests.sh`, `extract_z80_timing.py`. `Resources/` holds the app icon.

## Build and run

```bash
swift build -c release            # both products
tools/bundle.sh                   # build/MacAPE.app (needed to get the icon / Info.plist)
tools/macape game.dsk /A /SYM obj/game.noi
```

`build/MacAPE.app` is a bundle (a folder), not an executable: launch it with `open`, `tools/macape` or
`build/MacAPE.app/Contents/MacOS/MacAPE`.

## Tests

There is no XCTest suite. Verification is `cpcrun` plus screenshots; run these before committing core changes:

```bash
R=/Users/drodri/Documents/WinAPE20B2/ROM
.build/release/cpcrun zex Tests/z80/zexdoc.com        # ~70 s; must report no "ERROR"; also zexall.com
for t in dbgtest mftest amxtest; do .build/release/cpcrun $t $R; done
.build/release/cpcrun steptest $R ~/Proyectos/boop/boop.dsk   # also logtest; needs a disc that boots a game
.build/release/cpcrun symtest <symbols file> <symbol name>
```

`disctest` (a BASIC program writes a file during a session; replay/seek/clone must reproduce state and disc), `sotest` (reverse Step Over + markers) and
`statetest` (save/restore at 9 points incl. mid-frame, 200 frames each) and `rectest` (record boop with every event
type, replay in another machine, 80 random seeks, injected input must be detected, edit cuts the recording) are the
exactness tests for the savestate/replay work: run them after touching anything in `CPCCore` that holds state.

Other `cpcrun` commands: `boot` (renders a frame to BMP; options `--disc --type --wait --space --return --frames
--wav --out`; env `VIEW=raster|vram`, `MIDFRAME=<cycles>`, `DUMP=1`, `IOLOG=1 IOFROM=<frame>`), `dis`, `inttest`,
`tapetest`, `snaptest`, `newdisc`. `~/Proyectos/boop` (a CPCtelera game, its `.noi`/`.map` in `obj/`) is the usual
real-world test case.

UI checks run the app with hidden test flags, then read the PNG: `--panel <selector:> --panelat <s> --shotwin "<window title>"
--shot out.png --after <s> --type "text~"` (`~` = Return). `bitmapImageRepForCachingDisplay` renders some AppKit
controls blank (tab strips, segmented controls); use `screencapture` when the real look matters.

## Where things come from

- ROMs: `~/Documents/WinAPE20B2/ROM` (override with `[MacAPE] ROM Folder`). Per-user data lives in
  `~/Library/Application Support/MacAPE/` (`MacAPE.ini`, `Profile/`, `Help/`, `WINAPE.POK`).
- Help: pages extracted from `WinAPE.chm` with `extract_chmLib` (brew `chmlib`) into `Help/`; the viewer is `Help.swift`.
- WinApe.exe addresses worth knowing (Ghidra, image base 0x400000): AMX mouse per-frame logic 0x5989FA,
  palette intensity tables 0x5CCA14 (linear) / 0x5CCA44 (non-linear), INI key table 0x5CBCA4.
- `WINAPE.POK` format is decoded in `Pokes.swift` (header comment); it round-trips byte for byte.
- Debugger window layout and shortcuts mirror WinAPE; the guide is in the owner's Docs ("MacAPE — Guía del depurador").

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
  state is the whole `CPC` struct image plus RAM, both frame buffers, FDC (pointers stored as indices), tape
  position, auto-type, Multiface RAM; media and presentation settings are not in it. Fields that are not machine
  state are listed once in `LIVE_FIELDS` in `state.c` (zeroed on save, kept on restore) — **a new pointer or host
  setting in `struct CPC` must be added there**, or the state hash and restores break. Padding bytes are part of the
  hash; `rectest` step 0 checks two independent machines hash equal.
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
  `noteUnreproducible` / `cpc_record_taint`, which ends the recording there. Recording files embed
  `sizeof(CPC)`: they only load with the build that wrote them.
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
- Temporary test harnesses in the app (env-var hooks) must be removed before committing: `grep -n "DBG[A-Z0-9]*TEST\|DBGB[0-9]"`.
- Commit messages: imperative summary, then a short paragraph of what and why.
- ROM images, `.dsk` files and WinAPE's own files (`WinApe.exe`, `CPC_Symbols.ttf`, the CHM) are not in the repo.

## Not done yet

Session record/playback is complete: core, File menu, transport bar (`Transport.swift`), reverse debugging (`ReverseDebug.swift`: Step Back
⌥F7, Step Back Over ⌥⌘F8, Reverse Continue ⌥F9, Run Back to Cursor ⌥F4, "Who Wrote/Read This?"), disc writes and markers (`CPC_MARK_TEXT`,
stored in the `.cpcrec` and saved back when edited). Integrated Maxam-compatible assembler
(menu item says "Not available yet"), disc/sector editor, Format Disc, Flip Disc, AVI/WAV/YM recording, Symbiface II (IDE/RTC/PS2 mouse), 4 MB Yarek RAM, printer, ZIP/RAR,
tape CSV, DSC/HDR discs, gamepad joystick. Debugger ideas not built: profiler, DeZog/VS Code protocol, rewind,
"who wrote this byte", memory access map, AY channel viewer, break on change. A web (WebAssembly) build was discussed:
the core is already portable C, the UI would be new.
