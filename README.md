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

2. Sirve la versión web precompilada por HTTP:

   ```sh
   python3 -m http.server 8080 --bind 127.0.0.1 --directory web
   ```

3. Abre <http://localhost:8080/>.

No necesitas Emscripten para usarla: el JavaScript y el WebAssembly compilados están incluidos en el repositorio. Sólo hace falta Emscripten para recompilar el núcleo después de modificar el código C; las instrucciones técnicas y los controles están también en [web/README.md](web/README.md).

## Instalar en un servidor web existente

BluePixel Web es una aplicación estática: no necesita Node.js, PHP, un proceso de emulación en el servidor ni Emscripten. Basta con copiar los archivos precompilados de `web/` a cualquier carpeta servida por tu web, por ejemplo `public_html/bluepixel/` o `/var/www/html/bluepixel/`.

Copia el **contenido** de `web/` conservando la estructura y los nombres, incluidos `index.html`, `app.js`, `style.css`, `icon.png`, `bluepixel.js`, `bluepixel.wasm` y la carpeta `roms/`. Puedes subirlos desde el panel de tu hosting/FTP o, si tienes SSH y `rsync`, desde la raíz del repositorio:

```sh
rsync -av web/ usuario@servidor:/ruta/de/tu/sitio/bluepixel/
```

Después abre `https://tu-dominio/bluepixel/`. El servidor debe ofrecer los archivos WebAssembly con el tipo MIME `application/wasm` y servir la web por HTTPS (o HTTP en una red local); no abras `index.html` como archivo `file://`. Si el navegador informa de un error al cargar WebAssembly, revisa el tipo MIME de `bluepixel.wasm` en la configuración del servidor o consulta con el proveedor del hosting.

Para actualizar la instalación, vuelve a copiar el contenido actualizado de `web/` a esa misma carpeta. El despliegue de GitHub Pages se actualiza automáticamente, pero los otros servidores no: hay que subir allí los archivos de nuevo.
