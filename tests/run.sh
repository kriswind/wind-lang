#!/bin/sh
# Run from the repository root: sh tests/run.sh
#
# tests/ok/*.wind  must run successfully and print exactly the matching .out file.
# tests/err/*.wind must fail `wind check` with exactly the matching .err output.

WIND=build/wind
failed=0
total=0

for src in tests/ok/*.wind; do
    total=$((total + 1))
    actual=$("$WIND" run "$src" 2>&1; echo "[exit $?]")
    expected=$(cat "${src%.wind}.out"; echo "[exit 0]")
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

echo "$((total - failed))/$total tests passed"
[ "$failed" -eq 0 ]
