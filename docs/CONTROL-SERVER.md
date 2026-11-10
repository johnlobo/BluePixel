# Servidor de control local (pasos 16 y 17)

El proceso headless y la app macOS ofrecen CPCApi (los transportes llegaron con la 1.3; la versión actual es la 1.4) por socket Unix y WebSocket.
El servidor está **apagado por defecto**. El transporte stdio de headless sigue funcionando igual.
No hay escucha en interfaces de red externas: WebSocket se enlaza exclusivamente a `127.0.0.1`.

## App macOS

Activa **Settings → Control Server → Enable Local Control**. **Connection Info…** muestra el socket,
el puerto WebSocket elegido y la ruta del fichero de token. Desactivar la opción cierra las conexiones
y elimina el socket creado por el servidor. La opción no se guarda como preferencia de arranque.

También se puede activar explícitamente al lanzar la app:

```sh
tools/bluepixel --control-server --control-websocket 8765 \
  --control-origin http://localhost:8080
```

Opciones adicionales:

| Opción | Valor por defecto |
|---|---|
| `--control-unix PATH` | `~/Library/Application Support/BluePixel/Control/control.sock` |
| `--control-websocket PORT` | `0`, puerto libre elegido por el sistema |
| `--control-token-file PATH` | `~/Library/Application Support/BluePixel/Control/token` |
| `--control-origin ORIGIN` | Ningún Origin de navegador autorizado; se puede repetir |

Las opciones de configuración no activan el servicio sin `--control-server`.
El token y el socket se crean con permisos `0600`; el directorio Control se crea con `0700`.
Un token existente solo se reutiliza si es un fichero regular del usuario actual, sin enlaces simbólicos
ni permisos de grupo/otros. Borrar el token con el servidor detenido permite generar uno nuevo.

Al activar el control, la máquina visible queda pausada y la salida de audio local se detiene.
`run.*` avanza el tiempo virtual de esa misma máquina; no se crea otro emulador oculto.
`debug.resume` limpia una parada del depurador para el siguiente `run.*`, sin iniciar reproducción en tiempo real.
Al desactivar el servidor se restablece la salida de audio y la máquina queda pausada; usa Run para continuar.

Las peticiones de red se encolan al hilo de emulación y se ejecutan en serie bajo el lock del núcleo.
Durante ejecuciones largas se cede el lock entre fotogramas o lotes de instrucciones para que la interfaz
siga respondiendo. El worker de red no ejecuta CPCCore. Los controles existentes de la interfaz conservan
su sincronización: si el usuario cambia ajustes o memoria mientras una herramienta ejecuta, está cambiando
la misma máquina. Para una secuencia reproducible, deja la interfaz pausada y usa solo `run.*`.

No se puede activar el servidor durante grabación/replay; mientras está activo, esas acciones están deshabilitadas.
Los discos cargados por la API se conservan en memoria y se exportan con `disc.export`; no se guardan sobre
una ruta local de la UI. Antes de reemplazar un disco de la app se guardan sus modificaciones según las preferencias normales.

## Headless

```sh
tools/build-headless.sh
BLUEPIXEL_ROM_DIR="$PWD/web/roms" build/bluepixel-headless \
  --unix /tmp/bluepixel.sock --websocket 8765 \
  --token-file /tmp/bluepixel.token --origin http://localhost:8080
```

Se puede habilitar uno o ambos transportes. `--websocket 0` elige un puerto libre.
Con listeners, stdout imprime una línea JSON de disponibilidad (`server_ready` y `websocket_port`);
los errores van a stderr y los RPC viajan por los sockets, sin usar stdin.
Sin esas opciones, solo se usa stdio y no se crean listeners ni tokens.

## Cliente Unix

La primera línea debe ser `AUTH ` seguida del token de 64 caracteres hexadecimales y un salto de línea.
El servidor responde `OK\n`. Después se intercambian líneas JSON-RPC como en stdio:

```python
import json, socket
from pathlib import Path

token = Path('/tmp/bluepixel.token').read_text().strip()
client = socket.socket(socket.AF_UNIX)
client.connect('/tmp/bluepixel.sock')
stream = client.makefile('rwb')
stream.write(f'AUTH {token}\n'.encode()); stream.flush()
assert stream.readline() == b'OK\n'
stream.write((json.dumps({'jsonrpc': '2.0', 'id': 1,
                         'method': 'run.frames', 'params': {'count': 50}}) + '\n').encode())
stream.flush()
print(json.loads(stream.readline()))
client.close()
```

Una ruta Unix ya existente, incluso si parece un socket obsoleto, hace fallar el arranque.
El cierre solo elimina el inode que creó el servidor; no borra un fichero que haya reemplazado esa ruta.

## Cliente WebSocket

Endpoint: `ws://127.0.0.1:PORT/rpc` (también se acepta `/`). Cada mensaje de texto contiene una petición JSON-RPC;
cada respuesta es un mensaje de texto. Se admite fragmentación, ping/pong y cierre según
[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html).

Clientes nativos pueden usar la cabecera `Authorization: Bearer TOKEN` en el handshake.
Los navegadores, que no permiten fijar esa cabecera desde WebSocket, pueden autenticar con dos subprotocolos:

```javascript
const token = prompt('BluePixel token'); // contenido del fichero local de token
const ws = new WebSocket('ws://127.0.0.1:8765/rpc',
                        ['bluepixel.v1', `bluepixel.token.${token}`]);
ws.onopen = () => ws.send(JSON.stringify({jsonrpc: '2.0', id: 1,
                                         method: 'cpu.registers', params: {}}));
ws.onmessage = event => console.log(JSON.parse(event.data));
```

El servidor selecciona `bluepixel.v1`; no devuelve el token en el handshake ni lo coloca en la URL.
Si la petición incluye `Origin`, debe coincidir exactamente con uno de los autorizados, incluido esquema y puerto.
`http://localhost:8080` y `http://127.0.0.1:8080` son Origins distintos. Se rechazan `null`, comodines,
cabeceras de seguridad duplicadas y Host ajenos al listener local.
Un cliente nativo sin Origin sigue necesitando el token. No hay cookies ni autenticación implícita.

## Límites y cierre

- Peticiones: hasta 1 MiB; las respuestas de imagen/estado pueden ser mayores.
- Hasta 8 clientes, ejecución de RPC serializada sobre una máquina compartida.
- Handshakes/autenticación incompletos caducan a los 5 segundos. Las escrituras a clientes lentos tienen timeout.
- Texto UTF-8 y frames de cliente enmascarados; frames binarios o mal formados se cierran con el código correspondiente.
- Desactivar el servidor, cerrar la app o recibir SIGINT/SIGTERM cancela un `run.*` largo en una frontera de frame/instrucción,
  espera a que vuelva el callback y libera listeners/conexiones. Una ejecución interrumpida puede avanzar menos de lo pedido.
- `session.capabilities.transports` anuncia los transportes de la sesión: `stdio`, `unix`, `websocket` o `wasm`.

## Verificación

```sh
BLUEPIXEL_ROM_DIR="$PWD/web/roms" python3 Tests/headless/test_server.py
swift build -c release --product BluePixel
BLUEPIXEL_DESKTOP_BINARY="$PWD/.build/release/BluePixel" python3 Tests/headless/test_server.py
```

El segundo comando de prueba usa `BLUEPIXEL_DATA_DIR` en una carpeta temporal para no modificar los ajustes del usuario.
Se comprueban autenticación, Origin/Host, protocolo, frames cortos y largos, fragmentación, ping/pong, UTF-8,
límites, clientes concurrentes, reemplazo de medios, reinicio, permisos y cierre con un `run.frames` de un millón de frames en curso.
La versión nativa también se ejecuta bajo ASan/UBSan en CI.
