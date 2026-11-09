#!/bin/sh
# Builds CPCApi (and the core) to WebAssembly: web/bluepixel-capi.js + web/bluepixel-capi.wasm. Needs Emscripten.
# Not run in CI and not run on the Linux VPS (no emcc there): see docs/HEADLESS.md, "WASM over CPCApi".
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"
if ! command -v emcc >/dev/null 2>&1; then
  echo "Emscripten is required. Install it from https://emscripten.org/docs/getting_started/downloads.html" >&2
  exit 1
fi
OUT="${1:-$ROOT/web/bluepixel-capi.js}"
# CPCHost (files, roms.json, sha256) is deliberately absent: the browser hands ROMs and media over as JSON-RPC params.
emcc Sources/CPCCore/*.c Sources/CPCApi/api.c Sources/CPCApi/api_util.c Sources/CPCApi/api_media.c Sources/CPCApi/api_debug.c web/capi_exports.c \
  -std=gnu11 -D_DEFAULT_SOURCE -O3 -DNDEBUG \
  -I Sources/CPCCore/include -I Sources/CPCApi/include -I Sources/CPCApi -sUSE_ZLIB=1 -lz \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBluePixelCore \
  -sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,stringToUTF8,UTF8ToString,lengthBytesUTF8 \
  -sEXPORTED_FUNCTIONS='["_malloc","_free","_bpapi_create","_bpapi_destroy","_bpapi_dispatch"]' \
  -o "$OUT"
echo "Built $OUT"
