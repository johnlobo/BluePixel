# Fixture de aceptación para BluePixel: nave

Juego terminado de GlasnostFWK (`examples/nave`). Sirve para la rodaja vertical del headless.

| Fichero | Qué es |
|---|---|
| `nave.dsk` | Disco con `NAVE.BIN` (AMSDOS). Arranque: `run"nave` |
| `nave.bin` | Binario crudo, 8294 bytes, carga en 0x0200 |
| `nave.noi` | Símbolos (`DEF nombre 0xDIR`) |
| `fixture.json` | Direcciones, offsets de entidad, límites y matriz de teclado |
| `acceptance.json` | Guion de la rodaja vertical, paso a paso |
| `roms.json` | Manifiesto de ROM (solo el JSON: roles, tamaños y SHA-256 de las ROM de Amstrad; las ROM no van aquí) |
| `SHA256SUMS` | Sumas de control |

## Qué demuestra

1. Arranque de disco y `boot_ready` sin esperas por tiempo.
2. `game_status == 0x5A` marca el juego en marcha.
3. Teclado por matriz (el juego lee el hardware, no el buffer de firmware).
4. Lectura de memoria por símbolo: X del jugador en `game_storage+4`, Y en `game_storage+5`.
5. Límites: X 0..76 sin vuelta u8, Y 44..192.
6. Determinismo: dos ejecuciones en frío dan el mismo `state_hash` final.

## Verificación hecha

Los valores se comprobaron con AmSpiriT-Lite (6128, 128 KB): los símbolos coinciden con `fixture.json`, y manteniendo cada cursor el jugador se detiene en x=76, x=0, y=44 e y=192. Eso fija el comportamiento esperado, no el de BluePixel. No verificado: el reloj virtual, `boot_ready` y el hash, que dependen del nuevo emulador.

## Avisos

- Escribir en las posiciones de las entidades por RAM sin marcar sucio deja sprites fantasma en pantalla. Es del juego, no del emulador.
- En una captura tomada a media redibujada pueden faltar sprites. Capturar en el punto de sincronía.
- El contador de fotogramas del juego (`fwk_system_frame_count`) contiene basura de firmware antes de que cargue el juego: espera siempre a `game_status == 0x5A`.
