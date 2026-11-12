#!/usr/bin/env bash
# Builds cpcrun with plain cc and runs every core test that can run with material that is in the repository:
# the Z80 exercisers (zexdoc, zexall), the debugger/Multiface/AMX/interrupt/tape/snapshot tests, and the
# disc-driven exactness tests (step, logpoints, disc writes, savestates, record/replay) on the nave fixture.
#
#   tools/run-core-tests.sh                 everything (zexdoc + zexall run in parallel with the rest)
#   tools/run-core-tests.sh --skip-zexall   everything but zexall (the slowest test)
#   tools/run-core-tests.sh --only statetest [--only rectest ...]
#
# Environment: CC, CFLAGS (default -O2), BUILD_DIR (default build/core), ROM_DIR (default web/roms).
# Exit status is 0 only if every selected test passed. A test fails on a non-zero exit, on any "FAIL" line, on a
# "missing <rom>" message (the machine would run without firmware, which makes the result meaningless), on
# a failure count other than "0 failure(s)", and for the exercisers on any "ERROR" or an incomplete run.
# Full logs are in $BUILD_DIR/logs/<test>.log.
# Progress goes to stderr while the tests run (one line per finished test, and every 15 s which exercisers are
# still running); the summary goes to stdout at the end.
set -u
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/core}"
ROM_DIR="${ROM_DIR:-$ROOT/web/roms}"
Z80_DIR="$ROOT/tests/z80"
DISC="$ROOT/Tests/fixtures/nave/nave.dsk"
SYMS="$ROOT/Tests/fixtures/nave/nave.noi"
GAME=nave

SKIP_ZEXALL=0
ONLY=()
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-zexall) SKIP_ZEXALL=1 ;;
        --only) shift; ONLY+=("${1:?--only needs a test name}") ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
die() { echo "error: $*" >&2; exit 1; }
say() { printf '[%4ss] %s\n' "$SECONDS" "$*" >&2; }

# ---- inputs: ROMs (checked against the hashes pinned in roms.json) and the fixture disc ----
[ -f "$DISC" ] || die "missing fixture disc $DISC"
[ -f "$SYMS" ] || die "missing fixture symbols $SYMS"
[ -f "$ROM_DIR/roms.json" ] || die "no roms.json in $ROM_DIR"
for f in OS6128.ROM BASIC1-1.ROM AMSDOS.ROM OS464.ROM BASIC1-0.ROM; do
    [ -f "$ROM_DIR/$f" ] || die "missing ROM $ROM_DIR/$f"
    grep -q "$(sha256 "$ROM_DIR/$f")" "$ROM_DIR/roms.json" || die "$f does not match the hash pinned in $ROM_DIR/roms.json"
done

# ---- Z80 exercisers: downloaded once, verified against pinned hashes ----
ZEXDOC_SHA=34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f
ZEXALL_SHA=6e2da55147a04f28d303d5da6a1e6b771557ac244653590a0f24a2d39c8537e8
fetch_zex() {   # name, expected sha
    local f="$Z80_DIR/$1.com"
    if [ -f "$f" ] && [ "$(sha256 "$f")" = "$2" ]; then return 0; fi
    say "fetching $1.com"
    mkdir -p "$Z80_DIR"
    local try
    for try in 1 2 3; do
        curl -sfL --retry 2 --connect-timeout 15 --max-time 60 -o "$f.part" "https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/$1.com" \
            && [ "$(sha256 "$f.part")" = "$2" ] && mv "$f.part" "$f" && return 0
        sleep 2
    done
    rm -f "$f.part"
    die "could not fetch $1.com with the expected hash (see tools/fetch_z80_tests.sh)"
}

# ---- build ----
mkdir -p "$BUILD_DIR/logs"
rm -f "$BUILD_DIR"/logs/* "$BUILD_DIR/cpcrun"
CPCRUN="$BUILD_DIR/cpcrun"
say "building cpcrun ($CC $CFLAGS)"
# CPCHost/rom_files.c needs the CPCApi dispatcher and cpcrun does not use it, so only host_files.c is built.
"$CC" -std=gnu11 -D_DEFAULT_SOURCE $CFLAGS -DNDEBUG -pthread \
    -I"$ROOT/Sources/CPCCore/include" -I"$ROOT/Sources/CPCApi/include" -I"$ROOT/Sources/CPCHost/include" \
    "$ROOT"/Sources/CPCCore/*.c "$ROOT/Sources/CPCHost/host_files.c" "$ROOT/Sources/cpcrun/main.c" \
    -lz ${LDFLAGS:-} -o "$CPCRUN" || die "cpcrun does not build"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cpcrun-core.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ---- test table: name|kind|command ----
# kind: zex (exerciser), count (prints "N failure(s)" and must say 0), exit (exit status and FAIL lines only)
TESTS=(
    "zexdoc|zex|zex $Z80_DIR/zexdoc.com"
    "zexall|zex|zex $Z80_DIR/zexall.com"
    "dbgtest|count|dbgtest $ROM_DIR"
    "mftest|count|mftest $ROM_DIR"
    "amxtest|count|amxtest $ROM_DIR"
    "inttest|exit|inttest $ROM_DIR"
    "tapetest|exit|tapetest $ROM_DIR $WORK"
    "snaptest|exit|snaptest $ROM_DIR $WORK"
    "symtest|sym|symtest $SYMS _main"
    "sotest|count|sotest $ROM_DIR"
    "tracetest|count|tracetest $ROM_DIR"
    "steptest|count|steptest $ROM_DIR $DISC $GAME"
    "logtest|count|logtest $ROM_DIR $DISC $GAME"
    "disctest|count|disctest $ROM_DIR $DISC $GAME"
    "statetest|count|statetest $ROM_DIR $DISC $GAME"
    "rectest|count|rectest $ROM_DIR $DISC $GAME"
)
selected() {
    local n="$1" o
    [ ${#ONLY[@]} -eq 0 ] && { [ "$n" = zexall ] && [ $SKIP_ZEXALL = 1 ] && return 1; return 0; }
    for o in "${ONLY[@]}"; do [ "$o" = "$n" ] && return 0; done
    return 1
}
for o in ${ONLY[@]+"${ONLY[@]}"}; do
    ok=0; for t in "${TESTS[@]}"; do [ "${t%%|*}" = "$o" ] && ok=1; done
    [ $ok = 1 ] || die "unknown test '$o'"
done

selected zexdoc && fetch_zex zexdoc "$ZEXDOC_SHA"
selected zexall && fetch_zex zexall "$ZEXALL_SHA"

run_one() {     # name, command words...; writes log and "<seconds> <exit>" to .rc
    local name="$1"; shift
    local start=$SECONDS
    "$CPCRUN" "$@" >"$BUILD_DIR/logs/$name.log" 2>&1
    local rc=$?
    echo "$((SECONDS - start)) $rc" >"$BUILD_DIR/logs/$name.rc"
}

# verdict: prints "" when the log is clean, otherwise the reason
verdict() {     # name kind rc
    local log="$BUILD_DIR/logs/$1.log" kind="$2" rc="$3"
    [ "$rc" = 0 ] || { echo "exit status $rc"; return; }
    grep -q '^missing ' "$log" && { echo "ROM missing: $(grep -m1 '^missing ' "$log")"; return; }
    grep -q '^FAIL' "$log" && { echo "$(grep -m1 '^FAIL' "$log")"; return; }
    case "$kind" in
        zex)
            grep -q 'ERROR' "$log" && { echo "exerciser reported ERROR"; return; }
            grep -q 'Tests complete' "$log" || { echo "exerciser did not finish"; return; }
            [ "$(grep -c '\.\.  *OK$' "$log")" = 67 ] || { echo "expected 67 OK lines, got $(grep -c '\.\.  *OK$' "$log")"; return; } ;;
        count)
            grep -q '^0 failure(s)$' "$log" || { echo "no '0 failure(s)' line"; return; } ;;
        sym)
            grep -q '_main = 0B72;' "$log" || { echo "_main is not at 0B72"; return; } ;;
    esac
}
detail() {      # name kind: a short fact from the log for the summary line
    local log="$BUILD_DIR/logs/$1.log"
    case "$1" in
        zexdoc|zexall) echo "$(grep -c '\.\.  *OK$' "$log") exerciser groups OK" ;;
        inttest) grep '^[0-9]* interrupts' "$log" ;;
        symtest) grep -m1 'symbols;' "$log" ;;
        *) echo "$(grep -c '^ok:' "$log") checks ok" ;;
    esac
}

# the exercisers are the long pole: run them in the background while the quick tests run in sequence
result() {      # name kind: "PASS" or "FAIL: reason", from the finished test's log
    local secs rc why
    read -r secs rc <"$BUILD_DIR/logs/$1.rc" || { secs=0; rc=255; }
    why="$(verdict "$1" "$2" "$rc")"
    if [ -z "$why" ]; then echo "PASS (${secs}s)"; else echo "FAIL: $why"; fi
}
PIDS=(); BG=()
for t in "${TESTS[@]}"; do
    IFS='|' read -r name kind cmd <<<"$t"
    selected "$name" || continue
    [ "$kind" = zex ] || continue
    # shellcheck disable=SC2086
    run_one "$name" $cmd & PIDS+=($!); BG+=("$name")
done
[ ${#BG[@]} -eq 0 ] || say "started in the background (the slowest tests): ${BG[*]}"
for t in "${TESTS[@]}"; do
    IFS='|' read -r name kind cmd <<<"$t"
    selected "$name" || continue
    [ "$kind" = zex ] && continue
    # shellcheck disable=SC2086
    run_one "$name" $cmd
    say "$name: $(result "$name" "$kind")"
done
# wait for the exercisers, saying so every 15 s so a long wait is not a silent one (their logs are block-buffered,
# so a count of finished groups read from here would lag and mislead)
WAITED=0
while :; do
    alive=""
    for name in ${BG[@]+"${BG[@]}"}; do
        [ -f "$BUILD_DIR/logs/$name.rc" ] && continue
        alive="$alive $name"
    done
    [ -n "$alive" ] || break
    [ $((WAITED % 15)) -ne 0 ] || say "waiting for:$alive"
    sleep 1; WAITED=$((WAITED + 1))
done
for p in ${PIDS[@]+"${PIDS[@]}"}; do wait "$p"; done
for name in ${BG[@]+"${BG[@]}"}; do say "$name: $(result "$name" zex)"; done

# ---- summary ----
FAILED=0; COUNT=0
echo
echo "core tests ($(uname -s), $("$CC" --version 2>/dev/null | head -1))"
for t in "${TESTS[@]}"; do
    IFS='|' read -r name kind cmd <<<"$t"
    selected "$name" || continue
    COUNT=$((COUNT + 1))
    read -r secs rc <"$BUILD_DIR/logs/$name.rc" || { secs=0; rc=255; }
    why="$(verdict "$name" "$kind" "$rc")"
    if [ -z "$why" ]; then
        printf 'PASS  %-10s %4ss  %s\n' "$name" "$secs" "$(detail "$name")"
    else
        printf 'FAIL  %-10s %4ss  %s (log: %s)\n' "$name" "$secs" "$why" "$BUILD_DIR/logs/$name.log"
        FAILED=$((FAILED + 1))
    fi
done
echo "$((COUNT - FAILED))/$COUNT passed"
[ $FAILED = 0 ]
