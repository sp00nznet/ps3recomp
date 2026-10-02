#!/usr/bin/env python3
"""Regression test: a tail-entry must not snapshot a frame LOCAL as if it were a
callee-save restore.

Bug (Ben 10 Omniverse, UI layout fragment 0x4F5000 of 0x4F4F2C): the body has
no `stdu` (it is a gap/tail-entry of a bigger frame), stores the widget size
with an INDEXED vector store (`li r28,0xd0; stvx v1,r1,r28`) and reads it back
into callee-saved registers (`ld r27,0xd0(r1)`, `ld r25,0xd8(r1)`) to use it.
The callee-save pass saw an unpaired `ld r27,off(r1)` from a slot no
frame-offset store writes and rewrote it to a memory snapshot taken at fragment
ENTRY -- before the stvx -- so the layout read a stale size: the dialog band's
left half got pivot -0 instead of -208.5 and the whole band sat half a piece to
the right (c259.x 0 / 1.303 instead of -0.652 / +0.652).

Fix: the memory snapshot is only used for a load in a straight-line epilogue
(nothing reads the register before the body returns). The real epilogue
restores of the same body keep their snapshot.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")


def _insn(addr, mnemonic, operands):
    return SimpleNamespace(addr=addr, mnemonic=mnemonic, operands=operands)


def _lift(base, seq):
    insns = [_insn(base + 4 * i, m, o) for i, (m, o) in enumerate(seq)]
    lifter = L.PPULifter()
    func = lifter.lift_function(insns, base, base + 4 * len(seq))
    return "\n".join(func.body_lines)


def test_local_reload_after_indexed_store_is_not_snapshotted():
    body = _lift(0x4F5000, [
        ("li", "r28,0xd0"),
        ("stvx", "v1,r1,r28"),       # writes the size to r1+0xD0..0xDF
        ("ld", "r27,0xd0(r1)"),      # ...and reads it back: a LOCAL
        ("ld", "r25,0xd8(r1)"),
        ("std", "r27,0x90(r1)"),
        ("std", "r25,0x98(r1)"),
        ("ld", "r26,0x190(r1)"),     # genuine epilogue restores
        ("ld", "r31,0x1b8(r1)"),
        ("addi", "r1,r1,0x1c0"),
        ("blr", ""),
    ])
    assert "_cs_27" not in body, body
    assert "_cs_25" not in body, body
    assert "ctx->gpr[27] = vm_read64(ctx->gpr[1] + 0xD0);" in body.replace("0xd0", "0xD0"), body
    assert "ctx->gpr[25] = vm_read64(ctx->gpr[1] + 0xD8);" in body.replace("0xd8", "0xD8"), body
    # the real restores of the same tail-entry keep the entry snapshot
    assert "ctx->gpr[26] = _cs_26;" in body, body
    assert "ctx->gpr[31] = _cs_31;" in body, body


def test_epilogue_only_tail_entry_still_snapshots():
    body = _lift(0x10000, [
        ("ld", "r29,0x78(r1)"),
        ("ld", "r30,0x80(r1)"),
        ("ld", "r31,0x88(r1)"),
        ("addi", "r1,r1,0x90"),
        ("blr", ""),
    ])
    for r in (29, 30, 31):
        assert f"ctx->gpr[{r}] = _cs_{r};" in body, body
        assert f"uint64_t _cs_{r} = vm_read64(ctx->gpr[1] + " in body, body


def test_register_used_for_lr_is_a_plain_load():
    """`ld r31,0xb0(r1); mtlr r31` reloads the LR slot through r31: the value is
    used, so it stays a faithful load (the snapshot bought nothing there)."""
    body = _lift(0x26598, [
        ("ld", "r31,0xb0(r1)"),
        ("mtlr", "r31"),
        ("ld", "r31,0x98(r1)"),
        ("addi", "r1,r1,0xa0"),
        ("blr", ""),
    ])
    assert "ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0xB0);" in body.replace("0xb0", "0xB0"), body


def test_is_epilogue_restore_helper():
    lines = [
        "    ctx->gpr[27] = vm_read64(ctx->gpr[1] + 0xD0);",
        "    ctx->gpr[28] = vm_read64(ctx->gpr[1] + 0xD8);",
        "    ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x1C0);",
        "    { g_trampoline_fn = (void(*)(void*))func_00012345; return; }",
    ]
    assert L._is_epilogue_restore(lines, 0, 27)
    assert L._is_epilogue_restore(["    ctx->gpr[27] = vm_read64(ctx->gpr[1] + 0xD0);",
                                   "    return;"], 0, 27)
    # a call, a label or a branch after the load: not an epilogue
    assert not L._is_epilogue_restore(["x", "    ctx->lr = 0x1234; func_00001000(ctx); DRAIN_TRAMPOLINE(ctx);",
                                       "    return;"], 0, 27)
    assert not L._is_epilogue_restore(["x", "loc_00001000:", "    return;"], 0, 27)
    assert not L._is_epilogue_restore(["x", "    goto loc_00001000;"], 0, 27)
    # the register is read before the return
    assert not L._is_epilogue_restore(["x", "    vm_write64(ctx->gpr[1] + 0x90, ctx->gpr[27]);",
                                       "    return;"], 0, 27)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
