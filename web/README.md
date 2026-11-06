# BluePixel Web

The browser build reuses the portable C emulator core through WebAssembly. It currently supports a CPC 6128, bundled OS/BASIC/AMSDOS ROMs, DSK images in drives A and B, SNA snapshots, keyboard input, audio, and downloading disk images from both drives.

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
- Insert a DSK image into Drive A or Drive B, or load an SNA snapshot from the in-screen File menu.
- The CPC starts automatically after its default ROMs load. Click the screen to see the **F10** menu hint; the in-screen menu follows the macOS app structure (**BluePixel**, **File**, **Settings**, **Help**). Navigate with the arrow keys and Enter; use Left/Right for menu navigation. Press **F10** or **Escape** to close it. Click the screen and use the keyboard. Arrow keys also act as joystick directions; Control and Alt act as fire buttons.
- Sound is enabled by default and starts after the first click or key press, as required by browser audio policies. Toggle it from **Settings → Sound**.
- Disk writes are kept in memory independently for drives A and B. Use each drive’s **Download Changed Disc** menu item to save its DSK image.

The web UI is intentionally separate from the macOS AppKit frontend. Debugger windows, recording/replay, tapes, cartridges, and Plus-specific configuration are not part of this first browser version.

## Publishing

The repository includes a GitHub Pages workflow at `.github/workflows/deploy-pages.yml`. It compiles WebAssembly and deploys the contents of `web/` on every push to `main`; it can also be run manually from the Actions tab.

GitHub Pages sites are publicly accessible, including sites built from private repositories when the account plan permits it. This site bundle includes `web/roms/`, so those ROM files will also be publicly downloadable after deployment.
