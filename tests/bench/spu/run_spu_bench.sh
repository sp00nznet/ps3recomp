#!/usr/bin/env bash
# SPU operation benchmark: operations per second for every SPU operation,
# lifted and interpreted, built against the real SPU runtime.
#
#   tests/bench/spu/run_spu_bench.sh [--no-barriers] [harness args...]
#     harness args: --quick  --only fa,fm  --no-interp  --csv FILE
#
# Generated files go to $SPU_BENCH_OUT (default: tests/bench/spu/out).
# The lifted kernels are compiled with the flags the game compiles lifted SPU
# code with (-O2 -fwrapv; decomp/project/CMakeLists.txt).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${SPU_BENCH_OUT:-$HERE/out}"
CC="${CC:-clang}"
BARRIERS=1
if [ "${1:-}" = "--no-barriers" ]; then BARRIERS=0; shift; fi

rm -rf "$OUT"; mkdir -p "$OUT"
python3 "$HERE/gen_spu_bench.py" --out "$OUT"
python3 "$REPO/tools/spu_lifter.py" --auto-functions "$OUT/bench.elf" --output "$OUT/lift" > /dev/null
if [ $BARRIERS = 1 ]; then
    python3 "$HERE/apply_barriers.py" "$OUT/lift/spu_recomp.c" "$OUT/bench_cases.barriers"
else
    echo "barriers: none (the compiler may fold or hoist repeated ops)"
fi

S="$REPO/runtime/spu"
FLAGS=(-std=gnu17 -O2 -fwrapv -w -I "$S" -I "$REPO/include" -I "$REPO/runtime/platform" -I "$OUT")
"$CC" "${FLAGS[@]}" -include "$HERE/bench_barrier.h" -c "$OUT/lift/spu_recomp.c" -o "$OUT/lifted.o"
"$CC" "${FLAGS[@]}" -o "$OUT/spu_bench" "$HERE/spu_bench_main.c" "$OUT/lifted.o" \
    "$S/spu_channels.c" "$S/spu_drain.c" "$S/spu_lockstep.c" "$S/spu_coherency.c" \
    "$S/spu_workload.c" "$S/spurs_policy.c" "$S/spurs_job.c" "$S/spu_tsp_weak.c" \
    "$S/spu_vm_pagemap.c" "$S/spurs_policy_blob_weak.c" "$S/spu_interp.c" \
    "$REPO/runtime/platform/win32_compat.c" -lpthread -lm
"$OUT/spu_bench" "$@"
