# BluePixel

Emulador de Amstrad CPC 6128 para macOS y navegador.

## Usar la versión web

No hace falta instalar nada. Abre [BluePixel Web](https://johnlobo.github.io/BluePixel/) en un navegador moderno con WebAssembly y JavaScript activados. La web carga las ROMs incluidas automáticamente y empieza a emular cuando están listas.

- Haz clic en la pantalla para enfocarla y ver el aviso del menú, o pulsa **F10** para abrirlo. Usa las flechas y **Enter** para navegar; **F10** o **Escape** lo cierran.
- En **File → Drive A:** o **Drive B:** carga una imagen `.dsk` en cada unidad. Desde el mismo menú puedes descargar la copia modificada de cada disco.
- En **File → Load Snapshot…** carga un snapshot `.sna`.
- El sonido está activado por defecto. El navegador lo habilita después del primer clic o pulsación de tecla.
- En **Settings** puedes cambiar las ROMs, pausar o reiniciar la máquina y activar o desactivar el sonido.

Los cambios de disco permanecen en memoria durante la sesión. Descarga el disco desde su menú para conservarlos.

## Ejecutar una copia local

1. Clona el repositorio y entra en su carpeta:

   ```sh
   git clone https://github.com/johnlobo/BluePixel.git
   cd BluePixel
   ```

2. Instala Emscripten. En macOS:

   ```sh
   brew install emscripten
   ```

   Para otros sistemas, sigue las [instrucciones oficiales del SDK de Emscripten](https://emscripten.org/docs/getting_started/downloads.html).

3. Compila el núcleo WebAssembly y sirve la carpeta web por HTTP:

   ```sh
   tools/build-web.sh
   python3 -m http.server 8080 --bind 127.0.0.1 --directory web
   ```

4. Abre <http://localhost:8080/>.

El servidor HTTP es necesario: los navegadores no cargan el módulo WebAssembly desde una URL `file://`. Las instrucciones técnicas y los controles están también en [web/README.md](web/README.md).
