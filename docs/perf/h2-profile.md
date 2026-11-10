# H2 `nave` CPU profile

## Method

The supplied fixture checksums were verified with `shasum -a 256 -c SHA256SUMS`. The profile host was Ubuntu 24.04 x86_64 / GCC 13.3.0 running under QEMU on an Apple Silicon Mac, configured with four guest CPUs. It is Linux x86_64, but its CPU is emulated and its timings are not representative of the physical Haswell VPS.

`perf` could not find a matching tool for the VM kernel (`6.8.0-117`), so gprof was used. The profile executable was built with `CFLAGS="-O2 -pg" LDFLAGS="-static -pg"`; one cold acceptance run executed per profile. The H1 baseline worktree was at `ad9d5b8`; the H2 profile includes the inactive-debugger and idle-FDC optimizations. Percentages are gprof self time for each separate instrumented run, not production wall-clock shares.

The supplied VPS report is the physical Linux baseline: `-O2` took 5.1–5.6 s for 2,740 frames; `-O3 -march=native` took 4.5–4.8 s. Those numbers are supplied by the tester and are not measurements made here.

## Top ten functions

| Rank | H1 baseline | Self % | H2 final | Self % |
|---:|---|---:|---|---:|
| 1 | `cpc_step` | 32.54% | `cpc_step` | 32.21% |
| 2 | `cpc_video_tick` | 22.97% | `cpc_video_tick` | 26.86% |
| 3 | `psg_tick` | 10.36% | `psg_tick` | 12.61% |
| 4 | `z80_step` | 7.46% | `z80_step` | 9.15% |
| 5 | `fdc_tick` | 7.17% | `psg_mix` | 3.97% |
| 6 | `psg_mix` | 4.49% | `cpc_mem_fetch` | 3.80% |
| 7 | `tape_tick` | 3.48% | `fdc_tick` | 3.54% |
| 8 | `cpc_mem_fetch` | 3.48% | `cpc_run_frame` | 1.73% |
| 9 | `dbg_check_code` | 2.46% | `cpc_mem_read` | 1.12% |
| 10 | `cpc_mem_read` | 1.01% | `rot_shift` | 0.78% |

The active FDC tick count fell from 27,128,104 calls per cold run to 9,354,253. The gprof self-time estimate for `fdc_tick` fell from 0.99 s to 0.41 s across the separately instrumented runs. In the fixture, the tape is not loaded, so null tape ticks are skipped. With symbols loaded but no active breakpoints, `dbg_check_code` is no longer entered for every instruction.

The remaining profile is dominated by per-instruction CPU stepping and per-T-state video/PSG work. That points to the next substantial optimization area, but changing timing granularity there needs a new canonical-state and native/WASM conformance harness to protect behavior.

## Five-run benchmark

`Tests/headless/bench_nave.py` runs one cold acceptance sequence five times, including load, BASIC boot, keyboard-matrix movement, capture and restore replay. The benchmark was run on the same QEMU VM, first from a clean H1 worktree and then from H2, using the default `-O2` build. These are **emulated-CPU timings**, not VPS target results.

| Revision | Run factors | Median | Min | Max |
|---|---|---:|---:|---:|
| H1 (`ad9d5b8`) | 1.73x, 1.73x, 1.79x, 1.70x, 1.80x | 1.73x | 1.70x | 1.80x |
| H2 | 1.73x, 1.90x, 1.78x, 1.95x, 1.92x | 1.90x | 1.73x | 1.95x |

All ten runs returned H1-format hash `a8750fbe248c354e`. The median is approximately 9.8% higher under this emulator. This does not establish that the physical Linux VPS improved by the same amount, and it does not meet the 50x target.


## VPS real (Linux x86-64 4 vCPU, Intel Haswell): perfil y optimización exacta

Medido con `tools/measure-headless.py --profile` en la VPS física, `-O2` por defecto, estático, sin `-march=native`. Las dos
filas de la tabla son ejecuciones completas de la herramienta (aceptación, 5 arranques en frío y un arranque bajo Callgrind), sobre
árbol limpio: `8b97d18` (antes) y `7b62b6a` (después). El hash fue `be44e742783fb94c` en las 10 muestras.

| | Mediana | Mín | Máx | Instrucciones (Callgrind) |
|---|---:|---:|---:|---:|
| Antes (`8b97d18`) | 11,53x | 11,37x | 12,97x | 13.399 millones |
| Después (`7b62b6a`) | 20,99x | 19,17x | 22,37x | 6.944 millones |

Objetivo 50x por vCPU: **no alcanzado.** Mediana 21,0x. Las instrucciones bajan un 48% y el tiempo real un 45%.

Las diez funciones más caras, antes y después. Callgrind atribuye el trabajo inlinado a la función que lo contiene, así que
las filas no son comparables una a una (`cpc_video_tick` está ahora dentro de `step_inner`/`emit_char`, `cpc_step` dentro de `step_inner`):

| Antes | Ir | % | Después | Ir | % |
|---|---:|---:|---|---:|---:|
| `cpc_video_tick` | 6.134 M | 45,8 | `emit_char` | 2.215 M | 31,9 |
| `psg_tick` | 2.279 M | 17,0 | `step_inner` | 1.556 M | 22,4 |
| `cpc_step` | 1.707 M | 12,7 | `z80_step` | 1.277 M | 18,4 |
| `z80_step` | 1.176 M | 8,8 | `audio_advance` | 680 M | 9,8 |
| `cpc_mem_fetch` | 562 M | 4,2 | `psg_tick_slow` | 356 M | 5,1 |
| `fdc_tick` | 561 M | 4,2 | `audio_advance` (inline) | 190 M | 2,7 |
| `psg_mix` | 407 M | 3,0 | `cpc_run_frame` | 140 M | 2,0 |
| `cpc_run_frame` | 140 M | 1,0 | `cpc_mem_read` | 88 M | 1,3 |
| `cpc_mem_read` | 88 M | 0,7 | `cpc_mem_write` | 69 M | 1,0 |
| `cpc_mem_write` | 69 M | 0,5 | `fdc_tick` | 53 M | 0,8 |

### Qué se cambió (todo exacto)

1. **Vídeo**: `emit_char_fast` dibuja un carácter con tablas por modo y escrituras directas (máquinas no Plus, carácter dentro del
   buffer); la versión genérica queda para Plus y para los bordes. `cpc_video_tick` se inlina en el bucle de `advance`.
2. **PSG**: los niveles de canal y la mezcla estéreo se recalculan solo cuando algo que se oye cambia (un canal a amplitud 0 se
   queda en nivel 0 diga lo que diga su oscilador). Las cachés empiezan sucias (calloc, reset, estado restaurado, `cpc_configure`).
3. **Audio, cinta, grabación y FDC como deuda**: no generan interrupciones, así que `advance()` solo ejecuta vídeo y acumula
   `sub_debt`; `settle()` aplica esa deuda en un bucle, en el mismo orden por microsegundo, antes de cada acceso a un puerto
   (`cpc_io_read/write`), al devolver el control al host (`cpc_step`, `cpc_run_frame`) y al guardar, hashear o restaurar estado.
   En máquinas Plus se liquida en el acto (su DMA escribe registros de sonido desde el tick de vídeo).
4. **Búsqueda de opcodes** directa desde el mapa de memoria mientras no haya página ASIC visible ni un depurador con contadores
   de acceso o breakpoints de lectura (`Debugger.read_bps`). `symbols.load`, que usa el fixture, adjunta un depurador inactivo.
5. FDC en reposo: un solo test sobre los 4 flags de seek.

### Cómo se comprobó que no cambia nada observable

- Hash del fixture `nave` idéntico en cada paso, y en las 10 muestras de cada medición.
- `Tests/headless/test_render_golden.py`: 12 escenarios de vídeo (todos los modos, RAM y paleta aleatorias, paleta y modo cambiados a
  mitad de línea, geometrías CRTC parciales) y 13 de sonido (tonos, ruido, envolventes, registros cambiados a mitad, sonido de
  cinta, sondeo del bit de entrada de cinta, y variantes con `run.frames`). Los hashes cubren el estado y cada muestra de `audio.read`
  y se grabaron con el ejecutable anterior a las optimizaciones (`e4be45c`).
- `Tests/headless/fuzz_equivalence.py`: máquinas aleatorias (código Z80 aleatorio que escribe en los puertos del gate array, CRTC y
  PSG, modos, paletas, CRTC, interrupciones) en el ejecutable de referencia y en el actual: 1000 semillas idénticas en estado,
  registros y audio. Detecta las mutaciones que se probaron a propósito (147 de 300 semillas divergen con un recálculo de PSG
  eliminado).
- `cpcrun` `statetest`, `disctest`, `rectest`, `dbgtest`, `mftest`, `amxtest`, `sotest`, `tracetest`, `inttest`: salida idéntica byte a byte
  al ejecutable anterior (incluidos 2 fallos de `rectest` que ya existían: esperan otro juego).
- `check_state_coverage.py` (campos nuevos clasificados como derivados) y `test_state_abi.py`.

### Qué impide llegar a 50x y por qué no se ha hecho

Con las optimizaciones exactas quedan unas 253 instrucciones por microsegundo emulado (6.944 M / 27,4 M); 50x pide unas 110.
El resto de la ganancia está en no pintar píxeles que nadie va a ver, y el estado canónico lo impide: **`state.hash` y `state.save`
incluyen los dos buffers de fotograma** (`state.c`, "Both frame buffers"), así que cada fotograma debe pintarse entero para que el
hash coincida bit a bit. Para saltarse el pintado hace falta decidir una de dos cosas: (a) un modo opcional de `run.frames` que solo
pinte los últimos fotogramas de la llamada (el hash de esa ejecución sería otro, pero determinista y comparable entre plataformas), o
(b) sacar el buffer en curso del estado canónico (formato de estado 2, hash de referencia nuevo). La alternativa exacta, un vídeo
perezoso que difiere el pintado y lo reproduce al pedirlo, es mucho más trabajo y arriesga la exactitud.
