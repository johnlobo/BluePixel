# Retomar H2 en la VPS: rendimiento headless

Rama: `h2-core`. El siguiente objetivo es medir y perfilar el fixture `nave` en la VPS Linux x86_64 real,
y después optimizar hasta una mediana de 50x por vCPU conservando el comportamiento exacto.

## Preparar el entorno

```sh
git switch h2-core
git pull --ff-only
sudo apt-get update
sudo apt-get install -y build-essential zlib1g-dev python3 valgrind
```

Las ROMs originales vuelven a estar en `web/roms/`, con `roms.json`, por petición del propietario. La herramienta
las usa por defecto. No hace falta copiarlas ni fijar rutas del desktop. Para otras ROMs, define `BLUEPIXEL_ROM_DIR`;
para otro fixture, `BLUEPIXEL_FIXTURE_DIR`. El hash de referencia solo es válido con las ROMs del fixture.

## Medir antes de cambiar código

Con la VPS sin otras tareas pesadas:

```sh
python3 tools/measure-headless.py --profile --output build/perf/vps-before
```

La carpeta de salida debe ser nueva. Se construye el ejecutable con los defaults de `build-headless.sh`
(`-O2`, estático en Linux), se comprueba la aceptación completa y se miden cinco arranques en frío.
Después se construye un binario independiente con símbolos y se hace un arranque bajo Callgrind.
El perfil puede tardar varios minutos. Si solo necesitas medir tiempo, omite `--profile`.

Archivos que debes consultar:

- `host.json`: CPU, plataforma, commit, cambios locales, compilador y flags.
- `acceptance.log`: arranques en frío iguales, restauración/replay iguales y hash de referencia.
- `benchmark.json` y `benchmark.log`: cinco muestras, mediana, mínimo/máximo y si se llega a 50x.
- `callgrind.out` y `callgrind-functions.txt`: contadores de instrucciones y funciones más caras.
- `profile-run.json`: confirma el mismo hash bajo el profiler; sus tiempos no son el benchmark.

La aceptación completa ejecuta 2740 frames entre dos arranques. Cada muestra del benchmark ejecuta un arranque
de 1370 frames, con movimientos y replay tras restaurar. Se verifica `be44e742783fb94c` (estado canónico 1).
No comparar con `a8750fbe248c354e`, que corresponde al antiguo formato H1.

## Optimizar y comparar

Lee `docs/perf/h2-profile.md` como historial; sus perfiles QEMU no son medidas de la VPS. Empieza por el perfil nuevo.
Revisa `cpc_step`, vídeo/CRTC, Z80 y PSG solo en función de su coste medido. No cambies la temporización observable.

Después de una optimización:

```sh
BLUEPIXEL_ROM_DIR="$PWD/web/roms" python3 Tests/headless/test_api.py
BLUEPIXEL_ROM_DIR="$PWD/web/roms" python3 Tests/headless/test_api_ext.py
python3 tools/measure-headless.py --profile --output build/perf/vps-after
```

Los hashes deben seguir coincidiendo. Si hay Emscripten y Node disponibles, reconstruye WASM y ejecuta:

```sh
tools/build-web.sh
BLUEPIXEL_ROM_DIR="$PWD/web/roms" node Tests/wasm/conformance.mjs --wasm web/bluepixel-capi.js
node Tests/wasm/test_web_client.mjs
```

Publica en `docs/perf/h2-profile.md` las diez funciones más caras antes/después y la tabla real de velocidad.
El objetivo sigue siendo mediana ≥50x con los flags por defecto, sin `-march=native` por defecto.
Si no se alcanza, registra la mediana medida y el cuello de botella pendiente.

## Estado al entregar

- Web y headless comparten CPCApi; bridge antiguo eliminado y CPC 6128 con arranque automático.
- Chrome: nave cargado desde DSK, juego ejecutándose, teclado, depurador, paso y breakpoint comprobados.
- Prueba del adaptador WASM: ROMs, discos A/B y exportación, teclado/joystick, render/audio, SNA, cinta,
  símbolos, memoria, breakpoints/condiciones y liberación de buffers.
- Corregidos el límite int32 de símbolos en wasm32, la fuga al exportar discos y la devolución de errores de condiciones.
- Nativo macOS/WASM: 35/35 comparaciones iguales; aceptación nativa con hash de referencia y replay exacto.
- El CI de `54dda96` pasó Linux estático y WASM. La ampliación de CI en este cambio añade el adaptador, las ROMs
  reales y la herramienta de medición; consulta el resultado del último push antes de empezar.
- No se han hecho optimizaciones nuevas de rendimiento ni mediciones de la VPS desde el desktop.
- Siguen pendientes `boot_ready` por cabecera AMSDOS, servidor desktop opcional, Unix/WebSocket con token/Origin
  y adaptador de GlasnostFWK. No son necesarios para medir el proceso headless por stdio.

Prompt para retomar:

> Lee `docs/HANDOFF-VPS-H2.md` y `docs/HEADLESS.md`. Continúa en `h2-core`. Ejecuta la medición con Callgrind
> antes de tocar el núcleo, optimiza según el perfil y conserva el hash canónico. Documenta resultados reales;
> no declares el objetivo 50x alcanzado si la mediana no lo demuestra.
