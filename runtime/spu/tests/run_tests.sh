#!/usr/bin/env bash
# Build and run every integration test in this directory.
#
# Each test is a pair of files:
#   gen_test_<name>.py   - encodes the SPU program and wraps it in an ELF.
#   test_<name>_main.c   - harness with channel-overriding stubs and asserts.
#
# Usage: ./run_tests.sh  [<gcc-path>]
# Honours $GCC env var.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TOOLS="$REPO/tools"
RUNTIME_SPU="$REPO/runtime/spu"

GCC="${1:-${GCC:-gcc}}"

pass=0
fail=0
failed_tests=()

for gen in "$HERE"/gen_test_*.py; do
    name=$(basename "$gen" .py)
    name=${name#gen_test_}
    main="$HERE/test_${name}_main.c"
    [ -f "$main" ] || { echo "[skip] $name: no test_${name}_main.c"; continue; }

    elf="$HERE/test_${name}.elf"
    # Use an `out_` prefix for generated dirs so `gen_test_*.py` and
    # `out_*` never collide under a `rm -rf gen_*` glob.
    gen_out="$HERE/out_${name}"
    exe="$HERE/test_${name}.exe"

    rm -rf "$gen_out"
    python "$gen" > /dev/null
    python "$TOOLS/spu_lifter.py" --auto-functions "$elf" \
        --output "$gen_out" > /dev/null

    if "$GCC" -std=c11 -O2 -I "$gen_out" -I "$RUNTIME_SPU" \
        "$gen_out/spu_recomp.c" "$main" -o "$exe" \
        2> "$HERE/build_${name}.log"; then
        out=$("$exe" 2>&1) && rc=$? || rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "[PASS] $name -- $out"
            pass=$((pass+1))
        else
            echo "[FAIL] $name (exit $rc)"
            echo "$out" | sed 's/^/   /'
            failed_tests+=("$name")
            fail=$((fail+1))
        fi
    else
        echo "[FAIL] $name (build error -- see build_${name}.log)"
        failed_tests+=("$name")
        fail=$((fail+1))
    fi
done

echo
echo "==========================================="
echo "Results: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "Failed: ${failed_tests[*]}"; exit 1; }
