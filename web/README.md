# BluePixel Web

The browser build reuses the portable C emulator core through WebAssembly. It currently supports a CPC 6128, DSK images in drives A and B, CDT/TZX/VOC/WAV tape playback, SNA snapshot loading and saving, keyboard and gamepad input, audio, and downloading disk images from both drives. The bundled CPC 6128 OS, BASIC and AMSDOS ROMs load automatically at startup.

Firmware is included in `web/roms/`. User-loaded media stays in the browser on the user’s device.

## Ejecutar una copia local

Los archivos precompilados `bluepixel-capi.js` y `bluepixel-capi.wasm` están incluidos. No necesitas instalar Emscripten para usar la versión web. Desde la carpeta raíz del repositorio, inicia un servidor HTTP:

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

- CPC 6128 firmware loads automatically. Change the OS, BASIC or disk ROM from **Settings → Memory**.
- Insert a DSK image into Drive A or Drive B, or load/save the CPC state as an SNA snapshot from the in-screen File menu. Save disk images separately from each drive menu.
- Load CDT/TZX, VOC or WAV cassette files from **File → Tape**. Play, stop, rewind or remove the tape there; type `RUN""` or `LOAD""` on the CPC to load a program.
- Save a PNG of the CPC display with **File → Save Screenshot…**. Use **Settings → Machine → Full Screen** to enlarge the screen while preserving its 4:3 aspect ratio.
- **Settings → Display** selects colour, green or greyscale monitor output, screen width, and emulation speed from 50% to 500%. These choices are stored in browser local storage.
- Open the web debugger in its own window with **F7** or **Debug → Open Debugger…**; opening it pauses emulation. It shows Z80 registers and disassembly, supports single-step execution, code and memory read/write breakpoints, conditional expressions and pass counts, and includes a 256-byte editable memory view for the current CPU map. Example conditions: `A=#42`, `PC>=#C000`, or `value<>previous`. Closing the debugger window leaves the CPC paused until you choose **Continue**.
- The CPC starts after its required ROMs load. Click the screen to see the **F10** menu and **F7** debugger shortcuts; the in-screen menu follows the macOS app structure (**BluePixel**, **File**, **Settings**, **Debug**, **Help**). Navigate with the arrow keys and Enter; use Left/Right for menu navigation. Press **F10** or **Escape** to close it. Click the screen and use the keyboard. Arrow keys also act as joystick directions; Control and Alt act as fire buttons. Standard gamepads use the D-pad or left stick, with A/B as fire buttons. Touch screens get an on-screen joystick automatically; change its visibility in **Settings → Controls**.
- Sound is enabled by default and starts after the first click or key press, as required by browser audio policies. Toggle it from **Settings → Sound**.
- Disk writes are kept in memory independently for drives A and B. Use each drive’s **Download Changed Disc** menu item to save its DSK image.

The web UI is intentionally separate from the macOS AppKit frontend. Session recording/replay, cartridges, and Plus-specific configuration are not part of this browser version.

## Publishing

The repository includes a GitHub Pages workflow at `.github/workflows/deploy-pages.yml`. It deploys the precompiled contents of `web/` on every push to `main`; it can also be run manually from the Actions tab. Emscripten is not needed for deployment.

GitHub Pages serves the static files in `web/`. The bundle includes `web/roms/` and starts the CPC 6128 automatically.
