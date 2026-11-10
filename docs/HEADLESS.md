# BluePixel headless

Build on the current host with `tools/build-headless.sh`. The executable reads one JSON-RPC 2.0 request per line from stdin and writes one response per line to stdout through the `CPCServer` stdio transport. Network listeners are opt-in: see [CONTROL-SERVER.md](CONTROL-SERVER.md) for Unix sockets, loopback WebSocket and the desktop server. Use `--version` to print its version.

Input lines are limited to 1 MiB. Oversized lines receive a JSON-RPC invalid-request error and are drained before the next request.

## Probar en Linux

En Ubuntu 24.04 instala `build-essential` (gcc and libc headers), `zlib1g-dev` (including static `libz.a`) and `python3`. No cmake, Node.js or Emscripten is needed. Configure `BLUEPIXEL_ROM_DIR` to a directory with `roms.json` and the external ROM images; the `nave` fixture is in `Tests/fixtures/nave/` and is used by default; set `BLUEPIXEL_FIXTURE_DIR` only to test another copy.

```sh
LDFLAGS="-static" tools/build-headless.sh
python3 Tests/headless/test_api.py
python3 Tests/headless/test_acceptance_nave.py
```

`build-headless.sh` reads `CC` (default `cc`), `CFLAGS` (default `-O2`) and `LDFLAGS` (empty by default). The executable accepts `--rom-dir PATH`; otherwise it checks `BLUEPIXEL_ROM_DIR`, `roms/` beside the executable, then `~/.config/bluepixel/roms`. The tests read `BLUEPIXEL_ROM_DIR` (use `$PWD/web/roms` for the bundled firmware) and optionally `BLUEPIXEL_FIXTURE_DIR` (default `Tests/fixtures/nave/`), and do not contain fixed local paths.

Example:

```sh
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"session.capabilities","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"run.frames","params":{"count":50}}' \
  | ./build/bluepixel-headless
```

See [`api/schema.json`](../api/schema.json) for methods and data formats. Disk, CPC SNA snapshots, and exact machine state images are passed as `data_base64`, so the API dispatcher has no filesystem access. Use `snapshot.load` for a `.sna` file. Use `state.save` / `state.restore` for exact state round trips and for handing a running machine to another instance; the state is in the canonical format described under "Estado canónico" below and loads in any build. `screen.capture` returns raw 32-bit pixels in the core's `0xAARRGGBB` byte layout; clients must account for host byte order.

The dispatcher uses vendored [jsmn](https://github.com/zserge/jsmn), a small tokenizing JSON parser. Its MIT license is included in the header and [LICENSES/jsmn-MIT.txt](../LICENSES/jsmn-MIT.txt). It needs no allocation-heavy object model or runtime library dependency and can be compiled into both native and WASM targets.

File-based DSK and symbol helpers now live in `CPCHost`; `CPCCore` parses in-memory symbols and disk data only. Optional I/O tracing uses a host callback instead of writing to stderr from the core.

The firmware is bundled in `web/roms/` with a manifest, and is loaded automatically by the web frontend. The headless executable still loads firmware from a ROM directory or through the API; it does not embed it. The ROM directory contains a `roms.json` manifest with `machines.cpc6128.{os,basic,amsdos}`, each entry providing a basename, 16384-byte size and SHA-256. See [`roms.example.json`](../roms.example.json) and [`api/roms.schema.json`](../api/roms.schema.json). The headless loader rejects missing, wrong-sized or hash-mismatched ROMs before accepting commands. The API also accepts ROM bytes through `rom.load`.

`state.hash` is FNV-1a 64 over the canonical state stream, followed by the CRC of each mounted disc and the bytes of the loaded ROMs and cartridge. Restoring still expects the same media and ROM configuration; differences are reported as `hazards`.

The clock is virtual. Only `run.*` advances emulation. One NOP is four Z80 T-states; `run.nops` advances by whole instructions, so it can exceed the requested NOP count. `t_states_aligned` reports NOP-aligned counts. `run.frames` advances to successive CRTC flybacks.

`boot_ready.configure` currently requires an explicit entry address. It reports ready when the PC reaches that address and the core's text auto-type queue is drained. Automatic extraction of the AMSDOS program entry point from an inserted disk is not implemented in H1.

Run the H1 API smoke test with `python3 Tests/headless/test_api.py`. The Linux static build is intended to use a Linux x86_64 toolchain with static zlib; the build script accepts `CC` and `LDFLAGS` overrides. A successful macOS build does not establish Linux static compatibility.

On the available macOS arm64 host, after a 300-frame warm-up, a 1,000-frame BASIC idle batch took 0.428 seconds (19.968 virtual seconds, 46.6x). This was not the GlasnostFWK fixture and does not verify the Linux VPS target of 50x; the Linux static workflow has not run from this checkout.

The supplied `nave` fixture hashes matched `SHA256SUMS`. The full acceptance script loads the DSK and symbols, resets the CPC6128, waits for configured entry `0x0B72`, runs `RUN"NAVE"`, checks the game status and movement limits, captures the display, and compares cold-start and restored-state replays. On the available macOS arm64 host it passed with hash `a8750fbe248c354e` (H1 state format; the canonical format gives `be44e742783fb94c`, see below), equal cold-start hashes, and equal restore replay. It ran 2,740 virtual frames in 4.687 seconds (11.7x real time); `run.frames` accounted for 4.628 seconds, while input, memory, hash, state, capture and transport took 0.060 seconds. This is below the 50x Linux VPS target and is not a Linux measurement.

The web frontend now uses the same `CPCApi` dispatcher as headless; the old direct bridge has been removed. `boot_ready` currently requires a configured address; AMSDOS header auto-detection is pending. `screen.capture` returns the last completed frame in RGBA8 order. Passing `{"metadata":true}` additionally returns the screen mode, CRTC type and registers, hardware palette indices, and current ARGB palette.

H2 results and the Linux profiling method are recorded below; the canonical state format has its own section.

## H2

### Pasos 16 y 17: control local

**Implementados (API 1.3).** La app macOS ofrece un servidor apagado por defecto, activable desde Settings → Control Server o `--control-server`. Comparte la máquina visible mediante una sesión prestada; el worker de transporte encola los RPC al hilo de emulación. Unix y WebSocket comparten CPCApi también en headless. WebSocket solo escucha en `127.0.0.1`; ambos transportes requieren token, Unix usa permisos `0600` y los Origins de navegador deben estar autorizados de forma exacta.

Las pruebas con clientes reales cubren headless y desktop, incluidos permisos, tokens inválidos, Origins/Host, UTF-8, fragmentación, ping/pong, límites, carga de medios, clientes concurrentes y cierre/reinicio durante ejecuciones largas. El servidor cede el lock entre frames/lotes y cancela los `run.*` pendientes al cerrar. El camino stdio y la web se mantienen. Documentación y comandos: [CONTROL-SERVER.md](CONTROL-SERVER.md).


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

**HECHO** (formato de estado 1, API 1.1). Validado en Linux x86_64; macOS arm64 y wasm32 también coinciden en las 35 comparaciones del harness de conformidad (1000 frames), además de la aceptación nativa con hash de referencia.

**Formato.** `Sources/CPCCore/ser.h` es el único serializador. Cada parte de la máquina tiene una función de recorrido (`visit_*` en `state.c`, `fdc_state_visit`, `tape_state_visit`, `cpc_autotype_state_visit`) que llama a las primitivas `ser_u8/u16/u32/u64/int/bool/f32/f64` campo a campo, en un orden fijo. La misma función hace las cuatro cosas: escribir (`state.save`), hashear (`state.hash`), validar y leer (`state.restore`). No hay un segundo serializador ni una lista de campos aparte que mantener sincronizada. Se han borrado la imagen del struct C (`LIVE_FIELDS`, `scrub`) y los `*_state_save/restore` de FDC, cinta y autotipeo.

- Little endian, sin padding, `int` siempre de 32 bits, `bool` de un byte 0/1, floats como patrón IEEE-754. Nada depende del layout, del tamaño de puntero ni del orden de bytes del host.
- Cada sección empieza con una etiqueta de 4 bytes (`CFG0`, `Z80 `, `MEM0`, `ASIC`, `GA  `, `CRTC`, `BEAM`, `PPI `, `PSG `, `MISC`, `FDC0`, `TAP0`, `TYP0`, `DSK0`, `END `). El lector exige cada una, así que un desfase entre escritor y lector se detecta en el byte donde ocurre.
- Fichero (`state.save`): cabecera de 32 bytes (`"BPST"`, `u32 formato = 1`, `u64 frames`, `u64 cycles`, `u32 tamaño crudo`, `u32 tamaño zlib`) y después el flujo canónico comprimido con zlib. Los estados del formato H1 se rechazan.
- `state.hash`: FNV-1a 64 (el mismo algoritmo que en H1) del flujo canónico sin comprimir, seguido de la sección `MED0` (CRC de cada disco montado y bytes de las ROM cargadas y del cartucho).
- Restaurar valida el flujo entero antes de tocar la máquina (modo `CHECK`) y solo después lo aplica (modo `READ`). Un flujo corrupto se rechaza y la máquina queda exactamente como estaba.
- `state.restore` recibe bytes de un cliente, así que un estado bien formado pero con valores imposibles también se rechaza:
  - Todo `int` debe estar en ±2^24 y todo `float`/`double` debe ser finito. Así ningún contador puede desbordar en el siguiente tick.
  - Cada índice debe estar dentro de su array: pen del gate array (0..16), posición de desbloqueo del ASIC (0..16), registro del CRTC, volumen de la envolvente del PSG (0..15).
  - Cada dispositivo comprueba sus invariantes sobre una copia, antes de aplicar nada (`fdc_consistent`, `tape_consistent`):
    - FDC: posiciones de comando/resultado/ids de formato dentro de sus buffers; en lectura/escritura, buffer de sector presente y `buf_pos < buf_len`; sector, pista y disco presentes en los estados que los usan; ángulo y temporizadores acotados.
    - Cinta: bloque actual, inicio de bucle y pila de llamadas dentro de la cinta; `pidx` dentro de los pulsos del bloque, comprobado en un clon.
- Esto cierra todo lo que encontraron una revisión adversaria y la prueba de valores hostiles; todo era alcanzable desde `state.restore`:
  - 7 lecturas/escrituras fuera de rango o punteros nulos de la revisión adversaria.
  - Desbordamientos de contadores (`rot_us`, temporizadores, fase de audio).
  - El volumen de envolvente del PSG fuera de su tabla.
  - Un `fmt_count` negativo que escribía en la pista vecina dentro del mismo `CPCDisc`; ningún sanitizer lo ve en el momento de la escritura, solo al liberar después.
  - Una fuga en el formateo de pista restaurado a mitad de comando.

  El formato H1 tenía los mismos agujeros: aceptaba cualquier imagen del struct.
- Corregido de paso, aunque no lo introdujo este cambio:
  - El estado del autotipeo vivía en una tabla estática de 8 huecos indexada por puntero. Desde la novena máquina (el indexador de sesiones clona máquinas) varias compartían el hueco 0, y una máquina nueva en la dirección de una liberada heredaba su cola. Ahora es `CPC.autotype`, y `cpc_destroy` lo libera.
  - `snapshot.load` aceptaba un pen 17..31 desde un `.sna` y se salía de `ga_palette[17]`.
- Las grabaciones de sesión (`.cpcrec`, versión 4) usan enteros little endian y ya no comprueban `sizeof(CPC)`: cargan en cualquier build.
- Campos muertos borrados de `struct CPC`: `dummy_rom[16384]` y `disc[4]`. No se usaban, y los punteros de `disc[]` entraban en el hash de H1.
- La configuración de la que depende el estado (modelo, tipo de CRTC, RAM, Plus...) va en `CFG0`. Al restaurar se compara (`CPC_STATE_HAZARD_CONFIG`) pero no se aplica.

**Hash nuevo del fixture `nave`.** El hash final de `acceptance.json` pasa de `a8750fbe248c354e` (formato H1, imagen del struct, retirado) a **`be44e742783fb94c`** (formato 1). Vale para las ROM fijadas en el `roms.json` del fixture. `test_acceptance_nave.py` lo comprueba; `BLUEPIXEL_EXPECT_HASH` cambia el valor esperado, y vacío desactiva la comprobación. El test ahora recompila antes de probar, para no comprobar nunca un binario viejo; `BLUEPIXEL_NO_BUILD=1` prueba uno ya existente. El tercero debe actualizar su valor de referencia.

**Pruebas** (todas en este contenedor Linux x86_64):

| Prueba | Qué demuestra | Resultado |
|---|---|---|
| `Tests/headless/check_state_coverage.py` | Tres fases, todas bajo ASan/UBSan y LeakSanitizer. (1) Lee el layout de `struct CPC`, `struct FDC`, `struct CPCTape` y `struct CPCAutoType` de la información de depuración (`gdb ptype /o`) y cambia cada campo: todo byte que no esté en la lista razonada `NOT_STATE` debe llegar al flujo, y el valor debe sobrevivir a guardar y restaurar en otra máquina. Un campo nuevo que no se serializa ni está en la lista hace fallar la prueba. (2) 600 flujos corruptos (bits cambiados, bytes aleatorios, 0xFF, truncados): cada uno se acepta y se ejecutan 2 frames, o se rechaza sin cambiar la máquina. (3) Valores hostiles: de 1 a 4 campos al azar con valores extremos o de tipo índice, partiendo de tres puntos (FDC en reposo, a mitad de FORMAT y a mitad de READ DATA). Tras guardar y restaurar se fuerza la máquina: transferencias y ticks del FDC, pen y color, CRTC con la secuencia de desbloqueo del Plus, PPI/PSG, cinta, frames completos. 3000 estados por modelo (6128 y 6128 Plus); `HOSTILE_ITERATIONS` lo alarga. | PASS: 206 campos de estado, 17 de configuración, 67 que no son estado; 600 flujos corruptos, 0 fallos; 6000 estados hostiles (unos 4600 aceptados y ejercitados), 0 errores de sanitizer, 0 fugas. Una pasada larga de 40.000 estados hostiles también dio PASS |
| Mutaciones sobre la prueba anterior | Quitar un campo del serializador (`ga_hsync_cnt`, `fdc.rot_us`, `tape.elapsed_us`), truncar `crtc.ma_row` a 8 bits, quitar una cota (`ga_pen`, `unlock_pos`, `fmt_id_pos`), o hacer el FDC tan estricto que rechace estados reales | Las 8 detectadas: las de campo por la cobertura, las de cota por un error de UBSan en la fase hostil, la estricta por la prueba de cada frame (31 estados reales rechazados) |
| `Tests/headless/test_state_abi.py` | El mismo núcleo compilado con el layout normal y con `-fpack-struct=1` (`sizeof(CPC)` 17800 frente a 17698). Ejecuta nave hasta el frame 500 y 500 más; luego cada build restaura el estado de la otra (traspaso). | PASS: flujo canónico idéntico (1.807.673 bytes, sha256 `863ef4f835edbde8...`); hash `4133d0108cb9f00a` en el frame 500 y `d73b0c55123473e5` en el 1000 en ambas; traspaso en los dos sentidos con 0 hazards y el mismo hash final. Además guarda y restaura en cada uno de los 700 frames de la carga del disco (32 con el FDC a mitad de transferencia): 0 rechazados, 0 distintos. Así se comprueba que la validación no rechaza estados reales |
| `Tests/headless/test_api.py` | Guardar y restaurar exacto por la API; replay determinista | PASS |
| `Tests/headless/test_acceptance_nave.py` | Aceptación completa: dos arranques en frío iguales, replay tras restaurar igual, hash de referencia | PASS, `be44e742783fb94c` |
| `cpcrun statetest`, `rectest`, `disctest`, `sotest`, `tracetest`, con ROM reales y `nave.dsk` | Restaurar en 9 puntos (también a mitad de frame) y 200 frames idénticos; grabación, 80 seeks y clones; escrituras en disco durante una sesión | Igual que `main` antes del cambio: 0, 2, 0, 0 y 0 fallos. Los 2 de `rectest` (búsqueda inversa de watch y de breakpoint) ya fallaban en `main` con este disco |

`check_state_coverage.py` y `test_state_abi.py` corren en el CI de Linux. El CI usa ROMs desechables: el flujo y los hashes cambian, pero se sigue comprobando que coinciden entre layouts.

**Límites de la prueba histórica Linux.** `-fpack-struct` cambia offsets y padding, pero no el tamaño de los punteros ni el orden de bytes. La igualdad entre wasm32 y nativo es la batería de conformidad del punto 3. El contenedor original no tenía Emscripten ni macOS. La comparación macOS/wasm32 se ha completado después; no hay comprobación en host big endian. Riesgo conocido para comparar plataformas: el PSG guarda niveles `float` (`psg.level`, `audio_acc`). Si un compilador fusiona operaciones (FMA, que clang activa por defecto en arm64), el valor puede diferir en el último bit y con él el hash, aunque la máquina emulada sea la misma. Se verá en el punto 18.

### 3. Web sobre CPCApi

**HECHO para la migración a CPCApi y sus pruebas funcionales.** El frontend usa `web/capi-rpc-client.js`; el bridge C antiguo y sus binarios se eliminaron. Se mantienen los binarios `bluepixel-capi.js/.wasm` precompilados y las ROMs en `web/roms/`, con arranque automático del CPC 6128.

Verificación:

- Chrome local: cargar `nave.dsk` con F1, ejecutar `RUN"NAVE`, teclado, abrir F7, paso de instrucciones y parar en breakpoint.
- `node Tests/wasm/test_web_client.mjs`: ROMs, discos A/B y exportaciones repetidas sin fugas de buffers, arranque real del juego y movimiento por teclado, joystick, render/audio, SNA, cinta y depurador (símbolos, memoria, condiciones y pasos). Las pruebas de joystick verifican la API; no se ha usado un gamepad físico ni se ha comprobado audio a oído.
- `conformance.mjs --wasm`: 35/35 iguales con las ROMs fijadas durante 1000 frames en macOS arm64/wasm32, incluidos los bytes de estado.
- CI de `54dda96`: Linux estático y WASM aprobados. El job WASM ahora añade la prueba del adaptador y conformidad con ROMs reales; el de Linux ejecuta la herramienta reproducible de medición y guarda resultados como artefacto.

La prueba del adaptador encontró y corrigió tres problemas: el límite de símbolos int32 rechazaba valores válidos en wasm32 por usar un literal `long` sin signo; exportar discos no liberaba su buffer; errores de validación de condiciones se propagaban fuera del resultado esperado por la UI. Se conservan los nombres reales de los discos al cargarlos.

Para retomar el rendimiento en Linux real: [HANDOFF-VPS-H2.md](HANDOFF-VPS-H2.md), con `python3 tools/measure-headless.py --profile`.

Equivalencias `bridge.c` a CPCApi:

| `web/bridge.c` | CPCApi |
|---|---|
| `bp_init`, `bp_destroy` | `bpapi_create`, `bpapi_destroy` (`web/capi_exports.c`) |
| `bp_load_rom(slot, ...)` | `rom.load` (`role`: `os`=-1, `basic`=0, `amsdos`=7); `rom.unload` quita la ROM. Otros slots no tienen método |
| `bp_load_disc`, `bp_disc_name`, `bp_export_disc` | `disc.load` (`name` opcional), `disc.info`, `disc.export` |
| `bp_load_tape`, `bp_tape_play`, `bp_tape_rewind`, `bp_tape_remove`, `bp_tape_loaded/playing/name` | `tape.load`, `tape.play`, `tape.rewind`, `tape.remove`, `tape.status` |
| `bp_load_snapshot`, `bp_save_snapshot` | `snapshot.load`, `snapshot.save` |
| `bp_run_frame` (devuelve si el depurador paró) | `run.frames {count:1}`; el campo `break` es ese valor |
| `bp_reset` | `machine.reset` (también limpia una parada del depurador) |
| `bp_set_monitor` | `display.set_monitor` |
| `bp_type`, `bp_key`, `bp_joystick` | `input.text`, `input.key`, `input.joystick` |
| `bp_frame` (768x540, PAL + scanlines) | `screen.render` (por defecto lo mismo; `half_size` da 384x270) |
| `bp_read_audio` | `audio.read` (float32 little-endian intercalado L/R en base64; hasta 8192 fotogramas por llamada) |
| `bp_debug_attach`, `bp_debug_attached` | `debug.attach`, `debug.status` |
| `bp_debug_resume`, `_pause`, `_step_into`, `_step_out` | `debug.resume`, `debug.pause`, `debug.step_into`, `debug.step_out` (y `debug.run_to`) |
| `bp_debug_symbol_set/clear/count/value/name` | `debug.symbols.set`, `.clear`, `.list` |
| `bp_debug_mark_data`, `_clear_data` | `debug.data.mark`, `debug.data.clear` |
| `bp_debug_disassemble`, `_instruction_length` | `debug.disassemble` (`length` en cada instrucción) |
| `bp_debug_read_memory`, `_write_memory` | `debug.mem.read`, `debug.mem.write` |
| `bp_debug_register(id)`, `_tstates`, `_reset_tstates` | `cpu.registers` (incluye `im`, `iff1`, `iff2`, `halted`, `nops`), `run.reset_tstates` |
| `bp_debug_toggle_code_bp`, `_has_code_bp` | `debug.breakpoints.toggle_code` (devuelve `set`), `debug.breakpoints.list` |
| `bp_debug_breakpoint_count/_count_all/_address/_id/_type/_start/_end/_pass_count/_condition` | `debug.breakpoints.list` (`type` llega como texto: `code`, `mem_read`, `mem_write`, `io`, `change`, no como el número del enum) |
| `bp_debug_add_memory_bp(start, end, write)` | `debug.breakpoints.add_memory` (`access`: `read`, `write`, `readwrite`) |
| `bp_debug_set_condition`, `_last_error` | `debug.breakpoints.set_condition` (el motivo del rechazo viene en `error`) |
| `bp_debug_remove_breakpoint` | `debug.breakpoints.remove` |
| `bp_free`, `bp_release_disc_export` | no hacen falta (las respuestas son JSON) |

Diferencias con `bridge.c` a tener en cuenta al migrar (no hay test de paridad contra `bridge.c`; esta lista sale de leer ambos):

- `rom.load` exige exactamente 16384 bytes y solo conoce tres roles (os, basic, amsdos). `bp_load_rom` acepta cualquier tamaño y los slots -1 a 31.
- `tape.play` y `tape.rewind` devuelven error sin cinta; en `bridge.c` no hacían nada.
- El nombre por defecto de un disco es `<api>`, no `browser-a.dsk`/`browser-b.dsk`: pasa `name` en `disc.load` si la UI lo muestra.
- Una petición no puede pasar de 1 MiB por stdio (un DSK de más de unos 780 KB no cabe en base64 por ese transporte). En WASM no hay ese límite.
- `display.set_monitor` solo recolorea máquinas Plus (el 6128 ignora el monitor, igual que hoy con `bp_set_monitor`).
- `debug.step_into` y `debug.step_out` arman el depurador y la máquina solo avanza con `run.*`, como en `bridge.c`.
- `debug.attach` activa los breakpoints (`symbols.load` los deja desactivados). Resolver una dirección simbólica (`mem.read` con un nombre) también adjunta el depurador, así que `debug.status.attached` puede pasar a `true` sin llamar a `debug.attach`. El historial de ejecución, que la API no expone, queda apagado al terminar cada llamada.
- Los tipos de breakpoint llegan como texto, no como el número del enum.
- La API valida más que `bridge.c`: `set_condition` rechaza `pass_count` mayor que 1000000 y condiciones de más de 159 bytes (el depurador las truncaba en silencio), `debug.mem.read` rechaza longitudes de más de 4096 y direcciones que pasan de 0xFFFF (`bp_debug_read_memory` no tenía tope y daba la vuelta con `& 0xFFFF`), `input.key` rechaza filas mayores que 9 (antes se ignoraban) y los parámetros enteros que cambian estado (valor de un símbolo, ids) deben ser enteros.
- Las expresiones del depurador (`address: "(1+2)*3"`, condiciones) anidan como máximo 64 niveles; antes una petición con 20000 paréntesis mataba el proceso por desbordamiento de pila. Es un arreglo del núcleo (`dbg_eval`), no solo de la API.
- El bridge antiguo no compilaba contra el núcleo H2 y se eliminó al completar la migración. `tools/build-web.sh` reconstruye ahora el módulo CPCApi.
- `tools/build-wasm-capi.sh` fija `-sSTACK_SIZE=1048576`. Los 130 KB de tokens del parser y los 190 KB de `jsmn` viven en el heap, no en la pila, para no depender de la pequeña pila por defecto de Emscripten; la build y el harness se han ejecutado correctamente.

### 4. Pendientes H1

- **Matriz de teclado: HECHO en H1; confirmado.** `test_acceptance_nave.py` drives `input.matrix` with active-low row/column masks, and the actual game reaches all four movement limits.
- **Metadata de captura: HECHO.** `screen.capture` remains unchanged by default. With `metadata: true`, its response also includes `screen_mode`, `crtc_type`, 18 CRTC registers, 17 hardware palette indices and 17 current ARGB colours. The API smoke checks these fields.
- **Autodetección `boot_ready`: NO HECHO.** It still requires an explicit address; extracting the AMSDOS execution address from a file on a mounted DSK remains pending.

The Linux commands run in the QEMU x86_64 guest were:

```text
LDFLAGS="-static" tools/build-headless.sh         Built bluepixel-headless
./build/bluepixel-headless --version               bluepixel-headless 0.1.0-h1   (0.2.0 since state format 1)
python3 Tests/headless/test_api.py                 headless API smoke: PASS
python3 Tests/headless/test_acceptance_nave.py     PASS nave: hash=a8750fbe248c354e, cold_hash_equal=yes, restore_replay=yes
python3 Tests/headless/bench_nave.py               repeats=5, median=1.90x, min=1.73x, max=1.95x
```

ROMs stayed outside the repository and executable. The QEMU environment verified x86_64 Linux compilation, API behavior, fixture behavior and the hash invariant; it does not verify the physical Haswell performance target. Those were historical H1 measurements; native/WASM checkpoints are now verified as described in section 3.
