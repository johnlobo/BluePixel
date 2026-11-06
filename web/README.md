# BluePixel Web

The browser build reuses the portable C emulator core through WebAssembly. It currently supports a CPC 6128, bundled OS/BASIC/AMSDOS ROMs, DSK images in drive A, SNA snapshots, keyboard input, audio, and downloading the current disk image.

The `web/roms/` folder contains the default CPC 6128 firmware and the alternatives offered in the in-screen menu. They load automatically; ROMs can be changed from the in-screen menu while the emulator is running.

## Build and run

On macOS, install Emscripten with Homebrew. On other systems, use the [official Emscripten SDK instructions](https://emscripten.org/docs/getting_started/downloads.html). Then build and serve the `web/` folder:

```sh
brew install emscripten
tools/build-web.sh
python3 -m http.server 8080 --bind 127.0.0.1 --directory web
```

Open <http://localhost:8080/>. The web folder is self-contained, including its ROMs. A local HTTP server is needed because browsers do not load WebAssembly modules from `file://` URLs. `tools/build-web.sh` produces ignored `web/bluepixel.js` and `web/bluepixel.wasm` files.

## Controls

- The CPC 6128 OS, BASIC and AMSDOS ROMs load from `web/roms/` on startup. Change each ROM from the in-screen menu at any time.
- Insert a disk or load an SNA snapshot with the file pickers.
- The CPC starts automatically after its default ROMs load. Open the in-screen application menu with **F10**; navigate with the arrow keys and Enter, and use Left/Right to change ROMs. Press **F10** or **Escape** to close it. Click the screen and use the keyboard. Arrow keys also act as joystick directions; Control and Alt act as fire buttons.
- Enable sound from the page after a click, as required by browser audio policies.
- Disk writes are kept in memory. Use **Download disk** to save a new DSK image.

The web UI is intentionally separate from the macOS AppKit frontend. Debugger windows, recording/replay, tapes, cartridges, and Plus-specific configuration are not part of this first browser version.
