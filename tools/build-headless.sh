#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/bluepixel-headless}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"
mkdir -p "$(dirname "$OUT")"
set -- "$ROOT"/Sources/CPCCore/*.c
"$CC" -std=gnu11 -D_DEFAULT_SOURCE $CFLAGS -DNDEBUG -I"$ROOT/Sources/CPCCore/include" -I"$ROOT/Sources/CPCApi/include" -I"$ROOT/Sources/CPCServer/include" -I"$ROOT/Sources/CPCHost/include" "$@" "$ROOT/Sources/CPCApi/api.c" "$ROOT/Sources/CPCServer/stdio.c" "$ROOT/Sources/CPCHost/host_files.c" "$ROOT/Sources/CPCHost/rom_files.c" "$ROOT/Sources/CPCHost/sha256.c" "$ROOT/Sources/bluepixel-headless/main.c" -lz ${LDFLAGS:-} -o "$OUT"
printf 'Built %s\n' "$OUT"
