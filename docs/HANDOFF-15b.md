# Traspaso: paso 15b (WASM sobre CPCApi y migración de la UI web)

Estado de origen a 2026-11-10, `main` en `8247654` (PR #5). El paso 15b se retomó después en `h2-core`; resultados actuales en [HEADLESS.md, sección 3](HEADLESS.md#3-web-sobre-cpcapi). Este documento conserva el traspaso original y sus criterios.
Lo escribió una sesión de Claude en la VPS de Linux, donde no hay Emscripten, Swift ni navegador. Todo lo que aquí
se marca **sin verificar** nunca se ha ejecutado.

## Qué está hecho (verificado en Linux x86-64 estático)

- CPCApi 1.2: 57 métodos en `api/schema.json`, el contrato. Cubre todo lo que hace `web/bridge.c` (tabla de equivalencias y
  lista de diferencias de comportamiento en `docs/HEADLESS.md`, sección 3). `Tests/headless/test_api_ext.py` falla si el
  schema y el código listan métodos distintos.
- Núcleo: `dbg_eval` limita el anidamiento a 64 niveles (antes 20000 `(` mataban el proceso), `cpc_destroy` libera el
  Debugger, el parser de la API ya no se desborda con exactamente 8192 tokens.
- Hash de referencia del fixture `nave`: `be44e742783fb94c` (formato de estado 1). Velocidad en la VPS: 12 a 13x tiempo real
  (el objetivo es 50x, paso 13, sin empezar).
- Preparado pero **sin ejecutar** (no había `emcc`): `web/capi_exports.c`, `tools/build-wasm-capi.sh`,
  `Tests/wasm/conformance.mjs --wasm`. Del harness solo se ha ejecutado `--self-test` (nativo contra nativo, 35/35).

## Qué te toca (paso 15b), en orden

1. **Compilar a WASM.** Instala Emscripten y ejecuta `tools/build-wasm-capi.sh` (produce `web/bluepixel-capi.js` y
   `.wasm`). Es posible que pida ajustes de flags: el script nunca se ha corrido. Fija `-sSTACK_SIZE=1048576` por si la pila
   por defecto es pequeña (dato recordado, no comprobado). Exporta `_bpapi_create`, `_bpapi_dispatch`, `_bpapi_destroy`,
   `_malloc`, `_free` y los métodos de runtime `stringToUTF8`, `UTF8ToString`, `lengthBytesUTF8`, `HEAPU8`.
2. **Conformidad nativo contra WASM** (cierra la equivalencia wasm32 del paso 14). Necesitas un directorio de ROMs con un
   `roms.json` y los tres ROMs del fixture (nombres y hashes en `Tests/fixtures/nave/roms.json`: `os6128.rom` sha256
   `ce133ea1...`, `basic6128.rom` `58503070...`, `amsdos.rom` `ea65e0fb...`). Los ROMs no están en el repo ni deben
   estarlo.
   ```
   LDFLAGS="-static" tools/build-headless.sh        # en macOS: tools/build-headless.sh (sin -static)
   export BLUEPIXEL_ROM_DIR=/ruta/a/tus/roms
   node Tests/wasm/conformance.mjs --self-test                       # primero esto, debe dar 35/35
   node Tests/wasm/conformance.mjs --wasm web/bluepixel-capi.js      # lo que importa
   ```
   Criterio: 35/35 comparaciones iguales (10 puntos de `state.hash` cada 100 fotogramas, más `run.frames`,
   `cpu.registers` y los bytes de `state.save`). Si diverge, la sospecha principal es el PSG: guarda niveles `float`
   (`psg.level`, `audio_acc`) y una fusión FMA del compilador puede cambiar el último bit (clang lo activa por defecto en
   arm64). Prueba `-ffp-contract=off` antes de buscar otra causa.
3. **Migrar la UI.** `web/app.js` (importa `./bluepixel.js` en la línea 1 y crea el módulo en la 283) y
   `web/debugger-window.js` llaman a `core._bp_*` con `malloc`/`HEAPU8`. Pásalos a peticiones JSON-RPC sobre
   `bpapi_dispatch`; los binarios (ROM, DSK, SNA, cinta) viajan en `data_base64`. La tabla `bp_*` a método está en
   `docs/HEADLESS.md`. Cosas que cambian al migrar:
   - `bp_run_frame` devuelve si el depurador paró; ahora es el campo `break` de `run.frames`.
   - `bp_frame` renderiza 768x540 con PAL y scanlines; el equivalente es `screen.render` (los valores por defecto coinciden).
   - Audio: `audio.read` devuelve float32 little-endian intercalado en base64, hasta 8192 fotogramas por llamada.
   - Los tipos de breakpoint llegan como texto (`code`, `mem_read`, `mem_write`, `io`, `change`), no como el número del enum.
   - `rom.load` exige 16384 bytes exactos y solo conoce los roles `os`, `basic`, `amsdos`; `rom.unload` quita uno.
   - `tape.play` y `tape.rewind` dan error sin cinta (en `bridge.c` no hacían nada).
   - El nombre por defecto de un disco es `<api>`: pasa `name` en `disc.load` si la UI lo muestra.
4. **Borrar `web/bridge.c`**, regenerar los binarios y decidir si `web/bluepixel.js` y `bluepixel.wasm` se siguen
   versionando. `tools/build-web.sh` compila `bridge.c`, que **ya no compila** contra el núcleo (`CPC` no tiene miembro
   `disc`), así que el `bluepixel.wasm` actual no se puede regenerar desde `main`. Elimina o reemplaza ese script.
5. **Probar el navegador a mano** (nadie lo ha hecho desde antes de H2): arranque con ROMs, DSK en A y B, cinta, audio,
   snapshot, teclado/gamepad y todo el depurador (breakpoints con condición, pasos, símbolos, desensamblado).
6. **CI.** Añade un job con emsdk que construya el WASM y corra el paso 2 con ROMs desechables
   (`python3 Tests/headless/make_test_roms.py <dir>`). El job actual es `.github/workflows/headless.yml`.
7. **Documentación.** En `docs/HEADLESS.md` pasa la sección 3 de PARCIAL a HECHO con los números medidos, y quita los
   avisos de "sin verificar" que dejes verificados. Actualiza `CLAUDE.md` (sección "Not done yet" y estructura de `web/`).

## Trampas ya encontradas

- `symbols.load` deja los breakpoints desactivados; `debug.attach` los vuelve a activar.
- Un frame puede terminar antes de que el programa llegue a un breakpoint: en pruebas usa `run.frames` con `count` 3.
- El anillo de audio descarta muestras nuevas cuando está lleno: vacíalo (`audio.read` en bucle) antes de medir.
- Una petición no puede pasar de 1 MiB por stdio (un DSK de más de unos 780 KB no cabe en base64 por ese transporte).
  En WASM no hay ese límite.
- `display.set_monitor` solo recolorea máquinas Plus; el 6128 ignora el monitor (igual que `bp_set_monitor` hoy).
- Los tests de `cpcrun` quieren los nombres de ROM de WinAPE (`OS6128.ROM`...), si no imprimen `missing` y corren sin firmware.
- Los pasos de CI que reconstruyen `build/bluepixel-headless` deben llevar `LDFLAGS=-static`, o el último paso
  ("Verify static Linux executable") falla.
- Cualquier cambio de la API se anota en `api/schema.json` **y** en `Tests/headless/test_api_ext.py`.

## No verificado, para que no te sorprenda

- Todo el lado WASM (compilación, harness, navegador).
- Los targets Swift: `Package.swift` apunta a la carpeta `Sources/CPCApi/`, así que debería incluir los fuentes nuevos, pero no hay Swift en la VPS para comprobarlo.
- El hash `be44e742783fb94c` en macOS arm64 con el serializador canónico (el hash anterior `a8750fbe248c354e` sí coincidía
  entre macOS y Linux; el nuevo solo se ha medido en Linux). Es el paso 18.

## Después de 15b

Pasos pendientes del plan H2: (13) velocidad, de 12x a 50x por vCPU, con callgrind (`perf` no funciona en el contenedor);
(16) servidor en el desktop, apagado por defecto; (17) socket Unix, WebSocket, token y Origin; (18) comparación de hash
Linux/macOS/WASM; (19) adaptador de GlasnostFWK y CI con el fixture `nave`. El prompt completo de H2 está en
[prompts/bluepixel-h2-prompt.md](prompts/bluepixel-h2-prompt.md) (base reutilizable para H3).

## Cómo retomarlo con Claude en el desktop

Abre el repo en `main` actualizado y pega: "Lee `docs/HANDOFF-15b.md` y `docs/HEADLESS.md` sección 3. Empieza por el
paso 1 (compilar a WASM con `tools/build-wasm-capi.sh`) y avanza en orden; no des nada por hecho que el documento marque como
sin verificar."
