# Flujo de pruebas web con BluePixel (dsk + símbolos)

Fecha: 2026-11-17. Estado: diseño aprobado en conversación, pendiente de revisión del spec.

## Objetivo

Probar desde el navegador, contra este VPS, cualquier build de CPC con un clic y con
el depurador de BluePixel mostrando nombres de símbolos. Las pruebas internas de Claude
usan el BluePixel headless con los mismos ficheros.

Resultado medible: tras `cpc-publish`, el enlace impreso abre BluePixel, el juego
arranca sin teclear nada y F7 muestra etiquetas del `.noi` en el desensamblado.

## Estado actual

- `/config/www` (sin git, servido por un servidor externo a este contenedor): SPA estática
  con `rvmPlayer_cpc6128` del CDN (`index.html:161`), catálogo en `games.js`, discos en
  `gamez/`. Sin depurador ni símbolos.
- Publicación manual: `cp X.dsk /config/www/gamez/` (p.ej. `boop/code-server-compile.sh`)
  más edición a mano de `games.js`.
- `BluePixel/web`: build wasm con depurador (F7) y API de símbolos
  (`setSymbol`/`getSymbols`, `app.js:569`). Disco, símbolos y comando sólo por menú.

## Decisiones

1. BluePixel web **reemplaza** a rvmPlayer en toda la biblioteca.
2. Se embebe en la ficha del juego con un **iframe** (no se reescribe BluePixel como
   componente).
3. Publicar es un **script común** usado por todos los proyectos (Glasnost y no Glasnost).
4. El soporte de parámetros de URL va como **PR en BluePixel**.

## Pieza 1: parámetros de URL en BluePixel web (`web/app.js`)

Se leen de `location.search` de la página de BluePixel. Sin parámetros, comportamiento
idéntico al actual (GitHub Pages no cambia).

| Parámetro | Ejemplo | Efecto |
|---|---|---|
| `disk` | `../gamez/catdoku.dsk` | `fetch` + `core.loadDisc(0, …)` (unidad A) |
| `sym` | `../gamez/catdoku.noi` | `fetch`, parseo, `setSymbol()` por símbolo |
| `run` | `run"catdoku` | `core.typeText(run + "\n")` |
| `warp` | `1000` | ejecuta N frames con `core.runFrame()` sin dibujar, máx. 3000 (incluye el arranque) |

Orden, tras `loadDefaultROMs()` con éxito: disco, símbolos, warp de arranque (150 frames,
el mismo valor que `test_web_client.mjs`), `typeText`, resto del warp.

- Rutas resueltas con `new URL(valor, location.href)`; se rechaza todo origen distinto
  de `location.origin` (mensaje en `status`, no se carga nada).
- Símbolos: se usa el método `symbols.load` del core (`dbg_symbols_parse`, el mismo parser
  que la app nativa: `.noi`, `.map` de SDCC, RASM, sjasmplus), expuesto como
  `loadSymbols` en `capi-rpc-client.js`. No hay parser en JS.
- La lógica (parámetros, validación de origen, orden de arranque) va en `web/autoload.js`,
  sin DOM ni core, para probarla en Node.
- Errores de red o formato: mensaje en `status` y en el `hint`; el emulador sigue
  arrancado sin ese recurso.
- Docs: sección "Carga por URL" en `web/README.md`.

Tests: `Tests/wasm/test_autoload.mjs` (Node): parámetros, validación de origen, orden con
fakes y arranque real de nave con símbolos contra el core wasm. Paso añadido a CI.

## Pieza 2: `cpc-publish` (`/config/workspace/tools/cpc-publish`)

Python, sólo stdlib.

```
cpc-publish <id> <dsk> [--sym FICHERO.noi] [--run 'run"x'] [--title T] [--warp N] [--www DIR] [--sync-emulator]
```

- Copia a `<www>/gamez/<id>.dsk` y `<id>.noi`; escribe `<www>/gamez/<id>.json`
  (id, sha256 de dsk y noi, fecha, git sha y rama del proyecto si es repo).
- Rechaza (código 1) un `.noi` con mtime anterior al `.dsk`: un dsk y un noi de builds
  distintos etiquetan mal el código sin avisar.
- `games.js`: si existe la entrada `<id>`, actualiza `disk.url`, `symbols`, `command`;
  si no, añade una entrada mínima (`title` o `<id>`, `genre: 'Dev build'`).
  La edición es textual sobre el bloque del objeto con ese `id`; si el fichero no se
  puede parsear con seguridad, no escribe y lo dice.
- `--sync-emulator`: copia `BluePixel/web/{index.html,app.js,autoload.js,*.css,
  debugger*,bluepixel-capi.*,roms/}` a `<www>/bluepixel/`.
- Imprime la URL directa:
  `bluepixel/?disk=../gamez/<id>.dsk&sym=../gamez/<id>.noi&run=…&warp=…`.

Integraciones:
- `glasnost publish` (GlasnostFWK): construye, toma `build/<name>/<name>.dsk`, el `.noi`
  de `obj/` y el nombre AMSDOS del manifiesto, y llama a `cpc-publish`. Documentado en
  `docs/manual.html` (lo exige `tests/test_manual.py`).
- `boop/code-server-compile.sh` y equivalentes pasan a llamar a `cpc-publish`.

Tests: `unittest` sobre un `www` temporal: alta y actualización en `games.js`, noi viejo
rechazado, json con sha256 correcto, sync del emulador.

## Pieza 3: biblioteca (`/config/www/index.html`)

- `rvmPlayer_cpc6128(container, …)` y el `<script>` del CDN se sustituyen por
  `<iframe src="bluepixel/?…" allow="autoplay; fullscreen; gamepad">` construido desde
  la entrada de `games.js` (`disk.url`, `symbols` opcional, `command`, `warpFrames`).
- `destroyEmulator()` elimina el iframe (para wasm y audio).
- Clic sobre el iframe para el foco de teclado: texto de ayuda en la ficha.
- `CLAUDE.md` de `/config/www` actualizado (campo `symbols`, `cpc-publish`).

## Pruebas internas de Claude

`bluepixel-headless` con el mismo `.dsk` y `.noi` publicados: arranque, `run"`, N
frames, captura y lectura de RAM por símbolo. Un script en `cpc-publish --verify` no
entra en este alcance; primero se usa a mano y se codifica si se repite.

## Fuera de alcance

- Grabación/replay en web, CDT con símbolos, autenticación de la web.

## Riesgos

- `/config/www` y `/config/workspace/tools` no tienen git: un fallo de `cpc-publish`
  sobre `games.js` no se puede revertir. Mitigación: copia `games.js.bak` antes de
  escribir. Recomendado: `git init` en ambos (decisión del usuario).
- Ventana del depurador abierta desde un iframe: mismo origen, debería funcionar;
  se verifica en la implementación.
