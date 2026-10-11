#!/usr/bin/env bash
# Build and run the SPU interpreter selftest (includes the T-0001 scatter
# loop regression: the interpreter must run the inFamous WWS loop to its
# exact bound and keep every record inside the scratch span).
#   ./build_selftest.sh [cc]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${1:-${CC:-clang}}"
"$CC" -std=c11 -O2 -I "$HERE/.." "$HERE/spu_interp_selftest.c" \
    "$HERE/../spu_interp.c" -o "$HERE/spu_interp_selftest.exe" -lm
"$HERE/spu_interp_selftest.exe"
