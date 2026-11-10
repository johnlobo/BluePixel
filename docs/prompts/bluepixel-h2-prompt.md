> **Documento histórico.** Prompt original de H2 (2026-11-07), conservado como base reutilizable para H3. El texto de abajo está
> sin modificar. Estado actual y lo que queda: [HANDOFF-15b.md](../HANDOFF-15b.md) y [HEADLESS.md](../HEADLESS.md).
> Desde que se escribió:
>
> - El fixture `nave` ya está en el repo (`Tests/fixtures/nave/`); las ROM siguen fuera.
> - La rama de trabajo es `main`. `h1-headless` y `h2-core` ya están integradas.
> - El hash vigente del fixture es `be44e742783fb94c` (formato de estado 1). `a8750fbe248c354e` es el formato H1.
> - Objetivo 1 (velocidad): sin hacer, 12 a 13x en la VPS frente a 50x. Objetivo 2 (estado canónico): hecho, salvo la
>   comparación con macOS y WASM. Objetivo 3 (web sobre CPCApi): CPCApi 1.2 hecho y probado en nativo; el WASM y la
>   migración de la UI siguen pendientes. Objetivo 4: matriz de teclado y captura con metadatos hechos; la autodetección de
>   `boot_ready` sigue pendiente.

# BluePixel H2: velocidad, estado canónico y web sobre CPCApi

Trabajas en el repo BluePixel (emulador de Amstrad CPC, web + escritorio + servidor headless). Rama de partida: `h1-headless` (commit ad9d5b8). Crea la rama `h2-core` desde ella y trabaja ahí. No toques `main`.

Un tercero (GlasnostFWK, un framework de juegos CPC) probó H1 en su VPS Linux. Lo que sigue son sus mediciones y lo que necesita. Léelo entero antes de empezar.

## Resultado de la prueba de H1 en Linux

- Compilación estática: `LDFLAGS="-static" tools/build-headless.sh` en Linux x86-64 (Intel Haswell, 4 vCPU). Sale ELF estático, `--version` da `0.1.0-h1`. Correcto.
- `python3 Tests/headless/test_api.py`: PASS.
- `python3 Tests/headless/test_acceptance_nave.py`: PASS. Hash `a8750fbe248c354e`, arranques en frío iguales, repetición tras restaurar igual. Es el mismo hash que en macOS arm64: no cambies el comportamiento observable sin avisar.
- Rendimiento: 2740 fotogramas virtuales del fixture `nave` en 5,1 a 5,6 s con `-O2` (9,8x a 10,6x tiempo real). Con `-O3 -march=native`, 4,5 a 4,8 s (11,5x a 12,3x). De esos segundos, 5,07 s son `run.frames` y 0,3 s todo lo demás. El objetivo es 50x por vCPU. Hoy no se cumple y el cuello de botella es el núcleo de emulación, no el transporte.
- Tu cifra de 46,6x era con BASIC en reposo, no con el fixture. Para medir usa siempre el fixture `nave` y mide en Linux.

## Objetivos de H2 (en este orden)

### 1. Velocidad: 50x tiempo real por vCPU en el fixture `nave`, Linux x86-64

- Perfila primero (`perf record`/`perf report`, o `gprof`/`valgrind --tool=callgrind` si perf no está). Guarda el informe en `docs/perf/h2-profile.md` con las 10 funciones más caras antes y después. No optimices a ciegas.
- Candidatos a revisar, solo si el perfil lo confirma: bucle de decodificación Z80 (tabla de despacho, evitar cálculos de flags repetidos, tablas de paridad), acceso a memoria (puntero directo a bloques de 16 KB en lugar de decidir banco en cada acceso), temporización del gate array y CRTC (avanzar por eventos, no por ciclo de reloj), generación de vídeo (no renderizar píxeles en `run.frames` si no se pide captura; mantener solo el estado necesario para que la captura sea exacta), audio (si no hay consumidor, no mezclar muestras pero conservar el estado del PSG).
- Reglas duras: el hash de estado del fixture `nave` tras la secuencia de `acceptance.json` debe seguir siendo `a8750fbe248c354e`. Si una optimización lo cambia, o es un bug que introduces o es un cambio de comportamiento que debes explicar y justificar en el informe. Los tests Z80 existentes (`tools/fetch_z80_tests.sh`) siguen pasando.
- Añade un benchmark reproducible `Tests/headless/bench_nave.py` que imprima fotogramas, segundos, tiempo real (x) y repita 5 veces mostrando mediana y mínimo/máximo. El criterio de aceptación es la mediana ≥ 50x con el binario construido por `tools/build-headless.sh` con sus flags por defecto (puedes cambiar los flags por defecto de ese script, por ejemplo a `-O3`, si lo documentas; no uses `-march=native` por defecto porque el binario se distribuye).
- Si tras optimizar no llegas a 50x, no cambies el objetivo en silencio: informa de la mediana real, del perfil y de qué impide llegar.

### 2. Serializador de estado canónico

Hoy `state_hash` serializa el struct de C tal cual (el propio `docs/HEADLESS.md` lo reconoce). Eso rompe la equivalencia entre ABIs (wasm32 frente a nativo, padding, endianness).

- Escribe un único serializador campo a campo, little endian, sin padding, con versión de formato en la cabecera. Un solo código para: `state_hash`, guardar/restaurar estado y traspaso (handoff) entre instancias. No dejes dos serializadores.
- `state_hash` pasa a ser el hash (el mismo algoritmo que usas hoy, o SHA-256 truncado si lo cambias; documenta cuál) de ese flujo canónico.
- Pruebas: (a) ida y vuelta guardar/restaurar exacta; (b) el flujo de bytes canónico del fixture `nave` en un punto fijo (por ejemplo tras 500 fotogramas) coincide entre el binario nativo Linux y el de macOS si puedes ejecutar ambos, y entre nativo y WASM (ver punto 3). Si algún entorno no está disponible, dilo en el informe; no declares la equivalencia sin haberla medido.
- El hash cambiará respecto a `a8750fbe248c354e` porque cambia lo que se hashea. Eso es esperado: documenta el hash nuevo y deja el antiguo anotado como "formato H1". Actualiza `acceptance.json` de tu copia de pruebas si guardas hashes ahí, y avisa del nuevo valor en el informe para que el tercero actualice su fixture.

### 3. La web pasa por CPCApi

Hoy la web llama a un puente WASM directo antiguo. La arquitectura exige una sola semántica.

- Compila el mismo `CPCApi` (el despachador JSON-RPC 2.0 en C) a WASM y haz que la web lo use en lugar del puente directo. Borra el puente antiguo cuando la web funcione con el nuevo; no mantengas dos caminos.
- Prueba de equivalencia: una batería de conformidad que ejecute la misma secuencia de llamadas JSON-RPC contra el binario nativo y contra el WASM (en Node, sin navegador) y compare `state_hash` cada 100 fotogramas durante 1000 fotogramas del fixture `nave`. Los hashes deben coincidir en todos los puntos. Es la prueba que cierra el punto 2.
- La web debe seguir funcionando para un usuario normal (cargar un DSK, ejecutar, teclado). Describe cómo lo verificaste.

### 4. Pendientes pequeños de H1 (haz los que quepan, documenta los que no)

- `boot_ready` hoy exige una dirección configurada. Añade autodetección: si el fichero tiene cabecera AMSDOS con dirección de ejecución, úsala; si no, exige dirección explícita y devuelve un error claro.
- La entrada de teclado debe ir por la matriz del teclado del CPC (filas/columnas, activa a nivel bajo), no por inyección en el búfer del firmware. El fixture `nave` lo necesita: el juego lee la matriz directamente. Confirma que las pulsaciones del test de aceptación llegan por matriz.
- La captura devuelve hoy RGBA8 del último fotograma. Añade, sin romper lo existente, un modo opcional con paleta, modo de pantalla y registros del CRTC relevantes, para que un cliente pueda comprobar píxeles sin ambigüedad.

## Qué no hacer

- No rompas el comportamiento observable de H1 sin documentarlo (hash, API JSON-RPC, códigos de error). Si cambias un método de la API, súbele la versión y anótalo en `docs/`.
- ROMs: nunca dentro del repo ni del binario. Siguen en un directorio externo con `roms.json` (rol, fichero, tamaño, sha256). Orden de búsqueda: `--rom-dir`, `BLUEPIXEL_ROM_DIR`, `roms/` junto al ejecutable, `~/.config/bluepixel/roms`.
- El servidor sigue desactivado por defecto en la versión de escritorio, solo loopback, y solo un hilo toca el núcleo.
- No inventes mediciones. Cada cifra del informe sale de una ejecución que puedas pegar.

## Cómo comprobar tu trabajo (todo debe pasar en Linux x86-64, estático)

```sh
LDFLAGS="-static" tools/build-headless.sh
./build/bluepixel-headless --version
export BLUEPIXEL_FIXTURE_DIR=<carpeta del fixture nave>
export BLUEPIXEL_ROM_DIR=<carpeta con roms.json y las ROM>
python3 Tests/headless/test_api.py
python3 Tests/headless/test_acceptance_nave.py
python3 Tests/headless/bench_nave.py
```

El fixture `nave` (nave.dsk, nave.bin, nave.noi, fixture.json, acceptance.json, roms.json, SHA256SUMS) y las ROM no están en el repo: te los pasa quien te da este prompt. Verifica `SHA256SUMS` antes de usar el fixture.

## Informe final (en `docs/HEADLESS.md`, sección "H2", y en tu respuesta)

1. Estado por objetivo: HECHO, PARCIAL o NO HECHO, con la evidencia (salida pegada de los comandos de arriba).
2. Tabla de velocidad antes/después: mediana, mínimo y máximo de `bench_nave.py` en Linux x86-64, y las 10 funciones más caras antes y después.
3. Hash de estado nuevo del fixture `nave` y el viejo (formato H1).
4. Resultado de la batería de conformidad nativo frente a WASM (cuántos puntos comparados, cuántos iguales).
5. Qué sigue sin verificarse y qué lo resolvería.
6. Sube todo a `h2-core` y da el commit final.
