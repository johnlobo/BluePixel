# BluePixel

Emulador de Amstrad CPC 6128 para macOS y navegador.

## Usar la versión web

No hace falta instalar nada. Abre [BluePixel Web](https://johnlobo.github.io/BluePixel/) en un navegador moderno con WebAssembly y JavaScript activados. La web solicita los ficheros ROM necesarios y empieza a emular cuando están listos.

- Haz clic en la pantalla para ver los atajos **F10** (menú) y **F7** (depurador). Usa las flechas y **Enter** para navegar por el menú; **F10** o **Escape** lo cierran.
- En **File → Drive A:** o **Drive B:** carga una imagen `.dsk` en cada unidad. Desde el mismo menú puedes descargar la copia modificada de cada disco.
- En **File → Load Snapshot…** carga un snapshot `.sna`; usa **Save Snapshot…** para descargar el estado del CPC y retomarlo después. Los discos se guardan aparte desde el menú de cada unidad.
- En **File → Tape** carga cintas `.cdt`, `.tzx`, `.voc` o `.wav`; desde ahí puedes iniciar/detener la reproducción, rebobinar o retirar la cinta. En el CPC ejecuta `RUN""` o `LOAD""` para cargar el programa.
- En **File → Save Screenshot…** guarda una captura PNG del área de emulación. **Settings → Machine → Full Screen** muestra el CPC en pantalla completa sin deformar el formato 4:3.
- **Settings → Display** permite elegir monitor color/verde/grises, tamaño de pantalla y velocidad de emulación (50%–500%). Estas preferencias se guardan en el navegador.
- El sonido está activado por defecto. El navegador lo habilita después del primer clic o pulsación de tecla.
- El teclado, un gamepad estándar (cruceta o stick izquierdo; A/B para disparar) y los controles táctiles llevan el joystick del CPC. En pantallas táctiles aparecen automáticamente; puedes activarlos o desactivarlos en **Settings → Controls**.
- **F7** o **Debug → Open Debugger…** abre el depurador en una ventana independiente y pausa la emulación. Muestra registros Z80 y desensamblado, permite avanzar instrucción a instrucción, consultar/editar memoria y poner breakpoints de código o de lectura/escritura. Admite condiciones como `A=#42` o `value<>previous` y un contador de pasadas. Al cerrar la ventana, la máquina permanece pausada hasta que pulses **Continue**.
- En **Settings** puedes cambiar las ROMs, pausar o reiniciar la máquina y activar o desactivar el sonido.

Los cambios de disco permanecen en memoria durante la sesión. Descarga el disco desde su menú para conservarlos.

## Ejecutar una copia local

1. Clona el repositorio y entra en su carpeta:

   ```sh
   git clone https://github.com/johnlobo/BluePixel.git
   cd BluePixel
   ```

2. Sirve la versión web precompilada por HTTP:

   ```sh
   python3 -m http.server 8080 --bind 127.0.0.1 --directory web
   ```

3. Abre <http://localhost:8080/>.

No necesitas Emscripten para usarla: el JavaScript y el WebAssembly compilados están incluidos en el repositorio. Sólo hace falta Emscripten para recompilar el núcleo después de modificar el código C; las instrucciones técnicas y los controles están también en [web/README.md](web/README.md).

## Instalar en un servidor web existente

BluePixel Web es una aplicación estática: no necesita Node.js, PHP, un proceso de emulación en el servidor ni Emscripten. Basta con copiar los archivos precompilados de `web/` a cualquier carpeta servida por tu web, por ejemplo `public_html/bluepixel/` o `/var/www/html/bluepixel/`.

Copia el **contenido** de `web/` conservando la estructura y los nombres, incluidos `index.html`, `app.js`, `style.css`, `icon.png`, `bluepixel.js` y `bluepixel.wasm`. Puedes subirlos desde el panel de tu hosting/FTP o, si tienes SSH y `rsync`, desde la raíz del repositorio:

```sh
rsync -av web/ usuario@servidor:/ruta/de/tu/sitio/bluepixel/
```

Después abre `https://tu-dominio/bluepixel/`. El servidor debe ofrecer los archivos WebAssembly con el tipo MIME `application/wasm` y servir la web por HTTPS (o HTTP en una red local); no abras `index.html` como archivo `file://`. Si el navegador informa de un error al cargar WebAssembly, revisa el tipo MIME de `bluepixel.wasm` en la configuración del servidor o consulta con el proveedor del hosting.

Para actualizar la instalación, vuelve a copiar el contenido actualizado de `web/` a esa misma carpeta. El despliegue de GitHub Pages se actualiza automáticamente, pero los otros servidores no: hay que subir allí los archivos de nuevo.

## Emulador headless para Linux

La rama `h1-headless` añade una interfaz JSON-RPC por stdin/stdout para automatización y herramientas. En Ubuntu 24.04 instala `build-essential`, `zlib1g-dev` y `python3`; coloca las ROMs externas junto a un `roms.json` válido y configura `BLUEPIXEL_ROM_DIR`. Para ejecutar la aceptación con la fixture `nave`, configura también `BLUEPIXEL_FIXTURE_DIR`:

```sh
LDFLAGS="-static" tools/build-headless.sh
./build/bluepixel-headless --version
python3 Tests/headless/test_api.py
python3 Tests/headless/test_acceptance_nave.py
```

La guía completa, el protocolo y los formatos están en [docs/HEADLESS.md](docs/HEADLESS.md) y [api/schema.json](api/schema.json).
