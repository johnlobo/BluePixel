#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v emcc >/dev/null 2>&1; then
  echo "Emscripten is required. Install it from https://emscripten.org/docs/getting_started/downloads.html" >&2
  exit 1
fi

emcc Sources/CPCCore/*.c web/bridge.c \
  -std=gnu99 -D_POSIX_C_SOURCE=200809L -O3 -I Sources/CPCCore/include -sUSE_ZLIB=1 -lz \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createBluePixelCore \
  -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,HEAPF32 \
  -sEXPORTED_FUNCTIONS='["_malloc","_free"]' \
  -o web/bluepixel.js

echo "Built web/bluepixel.js and web/bluepixel.wasm"
