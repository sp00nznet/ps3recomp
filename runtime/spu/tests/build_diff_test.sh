#!/usr/bin/env bash
# Build (and optionally run) the SPU interpreter differential test against RPCS3's own SPU interpreter.
#   ./build_diff_test.sh [--run] [-- <test args>]
# Needs the local RPCS3 source tree (default: <repo>/../rpcs3, override with RPCS3_SRC=...) and a
# C++20 compiler (clang++ on Apple Silicon: RPCS3's simd.hpp maps SSE onto NEON via sse2neon.h).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
RPCS3="${RPCS3_SRC:-$HERE/../../../../rpcs3}"
OUT="$HERE/build_diff"
CC="${CC:-clang}"; CXX="${CXX:-clang++}"
mkdir -p "$OUT"
python3 "$HERE/rpcs3_oracle/extract.py" "$RPCS3" "$OUT/oracle_src.cpp"
"$CXX" -std=c++20 -O2 -w -ffp-model=strict -c -I"$HERE/rpcs3_oracle/shim" -I"$HERE/rpcs3_oracle" -I"$OUT" \
    -I"$RPCS3/rpcs3" -I"$RPCS3" -I"$RPCS3/3rdparty/asmjit/asmjit/src" "$OUT/oracle_src.cpp" -o "$OUT/oracle.o"
"$CC" -std=gnu11 -O2 -w -I"$HERE/.." -I"$HERE/rpcs3_oracle" -I"$OUT" -c "$HERE/spu_interp_diff_test.c" -o "$OUT/diff_test.o"
"$CXX" "$OUT/diff_test.o" "$OUT/oracle.o" -o "$OUT/spu_interp_diff_test" -lm
echo "built $OUT/spu_interp_diff_test"
# lifted mode: sample instruction words, lift each with tools/spu_lifter.py, compile that C into a second binary
"$OUT/spu_interp_diff_test" --emit-cases "${CASE_SCALE:-1}" > "$OUT/cases.txt"
python3 "$HERE/gen_lifted_cases.py" "$OUT/cases.txt" > "$OUT/lifted_cases.inc"
"$CC" -std=gnu11 -O2 -fwrapv -w -DLIFTED_CASES_INC=\"lifted_cases.inc\" -I"$HERE/.." -I"$HERE/rpcs3_oracle" -I"$OUT" -c "$HERE/spu_interp_diff_test.c" -o "$OUT/diff_test_lifted.o"
"$CXX" "$OUT/diff_test_lifted.o" "$OUT/oracle.o" -o "$OUT/spu_lifted_diff_test" -lm
echo "built $OUT/spu_lifted_diff_test  (run with --lifted)"
# --run: both modes. The single-precision float ops and conversions are judged
# against the SPU ISA (spu_float_referee.py, exact arithmetic) wherever RPCS3's
# model disagrees, since RPCS3's float model is not exact; everything else must
# match RPCS3 bit for bit.
if [ "$1" = "--run" ]; then shift; [ "$1" = "--" ] && shift
    rc=0
    for mode in interp lifted; do
        bin="$OUT/spu_interp_diff_test"; flag=""; [ $mode = lifted ] && { bin="$OUT/spu_lifted_diff_test"; flag=--lifted; }
        SPU_DIFF_SHOW=1000000 "$bin" $flag "$@" > "$OUT/run_$mode.log" 2>&1 || true   # mismatches go to the referee
        rg -v "MISMATCH|operands:" "$OUT/run_$mode.log" | tail -4
        python3 "$HERE/spu_float_referee.py" "$OUT/run_$mode.log" || rc=1
    done
    [ $rc = 0 ] && echo "SPU differential test: PASS" || echo "SPU differential test: FAIL"
    exit $rc
fi
