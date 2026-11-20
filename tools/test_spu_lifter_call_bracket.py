#!/usr/bin/env python3
"""Self-check: an SPU call bracket publishes its return pc for the whole call.

The interpreter stops at ctx->drain_ret_pc when a callee's return lands
mid-function. A bracket that set it only inside spu_drain_call left it stale
while the callee ran from the call itself, and the interpreter ran on through
the caller (Resistance 3: a job's loop nested a host frame per iteration until
the depth guard halted the SPU). runtime/spu/tests covers the runtime side.

Run: python tools/test_spu_lifter_call_bracket.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter  # noqa: E402

BASE = 0x1000
CODE = bytes.fromhex(
    "33000100"   # 0x1000 brsl $r0, 0x1008
    "35200180"   # 0x1004 bisl $r0, $r3
    "35000000"   # 0x1008 bi $r0
)


def check(body: str, ret: int, call: str, reloc: bool = False) -> None:
    line = next((ln for ln in body.splitlines() if call in ln), None)
    assert line, f"no bracket calling {call!r} in:\n{body}"
    # A relocatable lift (--reloc) publishes the return pc of the copy that runs.
    pc = f"((0x{ret:X}u + (uint32_t)ctx->reloc_delta) & 0x3FFFFu)" if reloc else f"0x{ret:X}"
    publish = f"ctx->drain_ret_pc = {pc};"
    assert publish in line, f"bracket does not publish {pc}: {line}"
    assert line.index(publish) < line.index(call), f"published after the call: {line}"
    restore = "ctx->drain_ret_pc = _dr;"
    assert restore in line and line.index(restore) > line.index("spu_drain_call"), (
        f"outer return pc not restored after the drain: {line}")


def main() -> int:
    insns = disassemble_spu(CODE, BASE)
    lifter = SPULifter()
    body = "\n".join(lifter.lift_function(insns, BASE, BASE + len(CODE)).body_lines)
    check(body, 0x1004, "spu_func_00001008(ctx)")
    check(body, 0x1008, "spu_indirect_branch(ctx)")
    lifter = SPULifter()
    lifter.reloc = True
    body = "\n".join(lifter.lift_function(insns, BASE, BASE + len(CODE)).body_lines)
    check(body, 0x1004, "spu_func_00001008(ctx)", reloc=True)
    check(body, 0x1008, "spu_indirect_branch(ctx)", reloc=True)
    print("ok: brsl and bisl brackets publish their return pc for the whole call (--reloc: the copy's)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
