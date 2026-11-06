# BluePixel Web

The browser build reuses the portable C emulator core through WebAssembly. It currently supports a CPC 6128, user-supplied OS/BASIC/AMSDOS ROMs, DSK images in drive A, SNA snapshots, keyboard input, audio, and downloading the current disk image.

The repository's `roms/` folder supplies the default CPC 6128 firmware and the alternative ROMs offered in the page's dropdowns. The browser frontend fetches them from the local server; custom files can also be selected in the page.

## Build and run

On macOS, install Emscripten with Homebrew. On other systems, use the [official Emscripten SDK instructions](https://emscripten.org/docs/getting_started/downloads.html). Then build and serve the repository root:

```sh
brew install emscripten
tools/build-web.sh
python3 -m http.server 8080 --bind 127.0.0.1
```

Open <http://localhost:8080/web/>. Serving the repository root lets the page load ROMs from `roms/`. A local HTTP server is needed because browsers do not load WebAssembly modules from `file://` URLs. `tools/build-web.sh` produces ignored `web/bluepixel.js` and `web/bluepixel.wasm` files.

## Controls

- The CPC 6128 OS, BASIC and AMSDOS ROMs load from `roms/` on startup. Change each ROM from its dropdown; custom files can be selected beneath each menu.
- Insert a disk or load an SNA snapshot with the file pickers.
- Click **Start**, then focus the screen and use the keyboard. Arrow keys also act as joystick directions; Control and Alt act as fire buttons.
- Enable sound from the page after a click, as required by browser audio policies.
- Disk writes are kept in memory. Use **Download disk** to save a new DSK image.

The web UI is intentionally separate from the macOS AppKit frontend. Debugger windows, recording/replay, tapes, cartridges, and Plus-specific configuration are not part of this first browser version.
