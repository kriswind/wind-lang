#!/bin/sh
# Run from the repository root: sh tests/run.sh
#
# tests/ok/*.wind  must run successfully and print exactly the matching .out file.
#                  A matching .in file, if present, is fed to the program's stdin.
#                  Each is run a second time built with --no-bounds-check, which
#                  must not change what a correct program prints.
# tests/err/*.wind must fail `wind check` with exactly the matching .err output.
# Both find "::" imports in tests/libs.
#
# tests/arduino/ok/*.wind are made into sketches with `wind emit-arduino`, and
#                  the sketch's C is built with tests/arduino/board.c, which
#                  stands in for the board, and must print the .out file.
# tests/arduino/err/*.wind must fail `wind emit-arduino` with the .err output.
# These import the libraries that come with wind.

WIND=build/wind
CC=${CC:-cc}
failed=0
total=0
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

WIND_LIBS=tests/libs
export WIND_LIBS

for src in tests/ok/*.wind; do
    total=$((total + 1))
    input="${src%.wind}.in"
    [ -f "$input" ] || input=/dev/null
    actual=$("$WIND" run "$src" 2>&1 <"$input"; echo "[exit $?]")
    expected=$(cat "${src%.wind}.out"; echo "[exit 0]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi

    total=$((total + 1))
    actual=$("$WIND" run --no-bounds-check "$src" 2>&1 <"$input"; echo "[exit $?]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s (--no-bounds-check)\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

for src in tests/err/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" check "$src" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.err"; echo "[exit 1]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

unset WIND_LIBS

for src in tests/arduino/ok/*.wind; do
    total=$((total + 1))
    input="${src%.wind}.in"
    [ -f "$input" ] || input=/dev/null
    rm -rf "$scratch/sketch"
    actual=$("$WIND" emit-arduino "$src" -o "$scratch/sketch" >/dev/null &&
        "$CC" -std=c11 -w -fwrapv -I tests/arduino -o "$scratch/board" \
            "$scratch/sketch/wind_program.c" tests/arduino/board.c &&
        "$scratch/board" <"$input" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.out"; echo "[exit 0]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

for src in tests/arduino/err/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" emit-arduino "$src" -o "$scratch/failed" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.err"; echo "[exit 1]")
    if [ "$actual" != "$expected" ] || [ -e "$scratch/failed" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

echo "$((total - failed))/$total tests passed"
[ "$failed" -eq 0 ]
