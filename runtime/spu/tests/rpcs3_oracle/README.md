# rpcs3_oracle: RPCS3's SPU interpreter as a test oracle (local only)

This directory lets `runtime/spu/tests/build_diff_test.sh` compare our SPU
interpreter and lifter against RPCS3's own SPU interpreter, op by op.

**It contains no RPCS3 code, and nothing built from it ever ships.**

- `extract.py` copies the interpreter's handlers out of a **local RPCS3
  checkout** at build time (`RPCS3_SRC`, default `../rpcs3` beside this
  repository) into `runtime/spu/tests/build_diff/oracle_src.cpp`. That
  directory is git-ignored; the extracted source and the binaries built from
  it stay on the machine that built them.
- `oracle.h`, `oracle_run.inc` and `shim/oracle_shim.h` are our own: the
  narrow C interface the test calls and the stand-ins for the parts of RPCS3
  the extracted handlers reference.
- Nothing in the runtime, the tools or any CMake target includes or links
  this directory. Only `build_diff_test.sh` uses it, and only when a local
  RPCS3 tree is present.

RPCS3 is GPL-2.0. Keep it that way: never commit `build_diff/`, never copy
extracted code into this directory, and never link the oracle into anything
but the differential test binaries.
