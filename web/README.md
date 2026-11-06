# BluePixel Web

The browser build reuses the portable C emulator core through WebAssembly. It currently supports a CPC 6128, bundled OS/BASIC/AMSDOS ROMs, DSK images in drives A and B, SNA snapshots, keyboard input, audio, and downloading disk images from both drives.

The `web/roms/` folder contains the default CPC 6128 firmware and the alternatives offered in the in-screen menu. They load automatically; ROMs can be changed from the in-screen menu while the emulator is running.

## Ejecutar una copia local

Los archivos precompilados `bluepixel.js` y `bluepixel.wasm` están incluidos. No necesitas instalar Emscripten para usar la versión web. Desde la carpeta raíz del repositorio, inicia un servidor HTTP:

```sh
python3 -m http.server 8080 --bind 127.0.0.1 --directory web
```

Abre <http://localhost:8080/>. El servidor HTTP es necesario porque los navegadores no cargan módulos WebAssembly desde URLs `file://`.

### Recompilar el núcleo (opcional)

Sólo hace falta Emscripten si modificas el núcleo C. Instálalo con Homebrew en macOS o sigue las [instrucciones oficiales del SDK de Emscripten](https://emscripten.org/docs/getting_started/downloads.html) en otros sistemas; después ejecuta:

```sh
tools/build-web.sh
```

## Controls

- The CPC 6128 OS, BASIC and AMSDOS ROMs load from `web/roms/` on startup. Change each ROM from the in-screen menu at any time.
- Insert a DSK image into Drive A or Drive B, or load an SNA snapshot from the in-screen File menu.
- The CPC starts automatically after its default ROMs load. Click the screen to see the **F10** menu hint; the in-screen menu follows the macOS app structure (**BluePixel**, **File**, **Settings**, **Help**). Navigate with the arrow keys and Enter; use Left/Right for menu navigation. Press **F10** or **Escape** to close it. Click the screen and use the keyboard. Arrow keys also act as joystick directions; Control and Alt act as fire buttons.
- Sound is enabled by default and starts after the first click or key press, as required by browser audio policies. Toggle it from **Settings → Sound**.
- Disk writes are kept in memory independently for drives A and B. Use each drive’s **Download Changed Disc** menu item to save its DSK image.

The web UI is intentionally separate from the macOS AppKit frontend. Debugger windows, recording/replay, tapes, cartridges, and Plus-specific configuration are not part of this first browser version.

## Publishing

The repository includes a GitHub Pages workflow at `.github/workflows/deploy-pages.yml`. It deploys the precompiled contents of `web/` on every push to `main`; it can also be run manually from the Actions tab. Emscripten is not needed for deployment.

GitHub Pages sites are publicly accessible, including sites built from private repositories when the account plan permits it. This site bundle includes `web/roms/`, so those ROM files will also be publicly downloadable after deployment.
