#!/bin/sh
# Lint gate used by .github/workflows/lint.yml and runnable locally.
#  1. every portable C source compiles with -Wall -Wextra -Wshadow -Werror (at -O0 and at the -O2 build-headless uses;
#     some diagnostics only exist with optimisation on),
#  2. the files listed in FORMATTED are exactly what clang-format (pinned: 23.1.5) would write.
# Overridable: CC, CLANG_FORMAT.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"
CC="${CC:-cc}"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

# Hand-formatted files (see CLAUDE.md, Conventions). CPCCore and the vendored jsmn.h are deliberately not listed.
FORMATTED="Sources/CPCApi/api.c Sources/CPCApi/api_util.c Sources/CPCServer/network.c Sources/CPCServer/stdio.c Sources/bluepixel-headless/main.c"

fail=0
for opt in -O0 -O2; do
    for f in Sources/CPCCore/*.c Sources/CPCApi/*.c Sources/CPCServer/*.c Sources/CPCHost/*.c Sources/bluepixel-headless/main.c Sources/cpcrun/main.c; do
        if ! "$CC" -std=gnu11 -D_DEFAULT_SOURCE $opt -DNDEBUG -pthread \
            -ISources/CPCCore/include -ISources/CPCApi/include -ISources/CPCApi \
            -ISources/CPCServer/include -ISources/CPCHost/include \
            -Wall -Wextra -Wshadow -Werror -c "$f" -o /dev/null; then
            fail=1
        fi
    done
done

# shellcheck disable=SC2086
if ! "$CLANG_FORMAT" --dry-run --Werror $FORMATTED; then
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "lint: FAILED" >&2
    exit 1
fi
echo "lint: ok ($("$CC" --version | head -n 1); $("$CLANG_FORMAT" --version))"
