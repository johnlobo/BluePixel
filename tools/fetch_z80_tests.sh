#!/bin/sh
# Downloads the public ZEXDOC/ZEXALL Z80 exercisers (not committed to the repo).
set -e
mkdir -p "$(dirname "$0")/../tests/z80"
cd "$(dirname "$0")/../tests/z80"
for f in zexdoc zexall; do
  curl -sfL -o $f.com https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/$f.com
done
