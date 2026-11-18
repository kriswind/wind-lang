#!/bin/sh
# Run from the repository root: sh tests/run.sh
#
# tests/ok/*.wind  must run successfully and print exactly the matching .out file.
#                  A matching .in file, if present, is fed to the program's stdin.
#                  Each is run a second time built with --no-bounds-check, which
#                  must not change what a correct program prints.
# tests/stop/*.wind must stop with status 1, printing exactly the matching .out
#                  file on stdout and stderr together: errors found while running.
# tests/err/*.wind must fail `wind check` with exactly the matching .err output.
# tests/warn/*.wind must pass `wind check` with exactly the matching .warn output.
# These find "::" imports in tests/libs.
#
# tests/arduino/ok/*.wind say "#target arduino", and are made into sketches with
#                  `wind build`, and
#                  the sketch's C is built with tests/arduino/board.c, which
#                  stands in for the board, and must print the .out file.
# tests/arduino/err/*.wind say "#target arduino", and must fail `wind check` with
#                  the .err output, and `wind build` the same way, writing nothing.
# These, and tests/std/*.wind, import the libraries that come with wind.
# bench/problems/*/main.wind must print expected-test.txt given "test".

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

for src in tests/stop/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" run "$src" 2>&1 </dev/null; echo "[exit $?]")
    expected=$(cat "${src%.wind}.out"; echo "[exit 1]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
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

for src in tests/warn/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" check "$src" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.warn"; echo "[exit 0]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

unset WIND_LIBS

# tests/std/*.wind use the libraries in libs/ and must print the .out file.
for src in tests/std/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" run "$src" 2>&1 </dev/null; echo "[exit $?]")
    expected=$(cat "${src%.wind}.out"; echo "[exit 0]")
    if [ "$actual" != "$expected" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

# bench/problems/*/main.wind, at test size, must print expected-test.txt,
# with bounds checks and without them.
for src in bench/problems/*/main.wind; do
    expected=$(cat "${src%/main.wind}/expected-test.txt"; echo "[exit 0]")
    for flag in "" --no-bounds-check; do
        total=$((total + 1))
        actual=$("$WIND" run $flag "$src" test 2>&1 </dev/null; echo "[exit $?]")
        if [ "$actual" != "$expected" ]; then
            failed=$((failed + 1))
            printf 'FAIL %s %s\n--- expected\n%s\n--- actual\n%s\n' \
                "$src" "$flag" "$expected" "$actual"
        fi
    done
done

for src in tests/arduino/ok/*.wind; do
    total=$((total + 1))
    input="${src%.wind}.in"
    [ -f "$input" ] || input=/dev/null
    rm -rf "$scratch/sketch"
    actual=$("$WIND" build "$src" -o "$scratch/sketch" >/dev/null &&
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
    actual=$("$WIND" check "$src" 2>&1; echo "[exit $?]")
    built=$("$WIND" build "$src" -o "$scratch/failed" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.err"; echo "[exit 1]")
    if [ "$actual" != "$expected" ] || [ "$built" != "$expected" ] || [ -e "$scratch/failed" ]; then
        failed=$((failed + 1))
        printf 'FAIL %s\n--- expected\n%s\n--- actual\n%s\n' \
            "$src" "$expected" "$actual"
    fi
done

echo "$((total - failed))/$total tests passed"
[ "$failed" -eq 0 ]
