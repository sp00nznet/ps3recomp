"""
selftest.py -- smoke test for the pure-Python harness core (no Ghidra/IDA/games).

Builds two tiny synthetic Modules that mimic the same program compiled for PS3
and X360 (shared strings, parallel call graph, slightly different codegen) and
asserts the matcher pairs them up via string anchors + call-graph propagation.

  python selftest.py
"""

from schema import Module, Unit, ARCH_CELL, ARCH_XENON, PLATFORM_PS3, PLATFORM_X360
from match_functions import match
from compare_report import build_findings, render_markdown


def mk(addr, name, calls, strs, mnem, leaf=False, consts=None, insn=None):
    return Unit(
        addr=addr, size=(insn or sum(mnem.values())) * 4, name=name,
        insn_count=insn or sum(mnem.values()), is_leaf=leaf,
        calls=calls, string_refs=strs, const_refs=consts or [],
        mnemonic_hist=mnem,
    )


def main():
    # ---- PS3 side -------------------------------------------------------
    a = Module(PLATFORM_PS3, "EBOOT.ELF", ARCH_CELL, "ppu_native", units=[
        mk(0x10000, None, [0x10100, 0x10200], ["Hedgehog::World::update tick"],
           {"std": 4, "lwz": 8, "bl": 2, "addi": 6, "blr": 1}),
        mk(0x10100, None, [0x10300], ["assert failed: %s:%d player not null"],
           {"lwz": 5, "bl": 1, "cmpwi": 3, "blr": 1}),
        mk(0x10200, None, [], ["shader/stage_lighting.fx compile"],
           {"lfs": 6, "fmuls": 4, "stfs": 4, "blr": 1}, leaf=True),
        mk(0x10300, None, [], [],  # leaf w/ no strings -> only reachable via callgraph
           {"li": 2, "blr": 1}, leaf=True),
    ])

    # ---- X360 side: same program, different addresses + slightly diff codegen
    b = Module(PLATFORM_X360, "default.xex", ARCH_XENON, "ghidra", units=[
        mk(0x82010000, "World::update", [0x82010100, 0x82010200],
           ["Hedgehog::World::update tick"],
           {"std": 4, "lwz": 7, "bl": 2, "addi": 5, "vmrghw": 1, "blr": 1}),  # +VMX128
        mk(0x82010100, "checkPlayer", [0x82010300],
           ["assert failed: %s:%d player not null"],
           {"lwz": 5, "bl": 1, "cmpwi": 3, "blr": 1}),
        mk(0x82010200, "compileLighting", [],
           ["shader/stage_lighting.fx compile"],
           {"lfs": 6, "fmuls": 4, "stfs": 4, "blr": 1}, leaf=True),
        mk(0x82010300, "clampHelper", [],
           [], {"li": 2, "blr": 1}, leaf=True),
    ])

    res = match(a, b, verbose=True)
    pairs = {(m.a_addr, m.b_addr): m for m in res.matches}

    expect = [
        (0x10000, 0x82010000, "string"),
        (0x10100, 0x82010100, "string"),
        (0x10200, 0x82010200, "string"),
        (0x10300, 0x82010300, "callgraph"),  # only reachable by propagation
    ]
    ok = True
    for aa, bb, _ in expect:
        if (aa, bb) not in pairs:
            print(f"  MISSING match {aa:#x} <-> {bb:#x}")
            ok = False

    print(f"\nmatched {len(res.matches)}/4 expected pairs")
    findings = build_findings(a, b, res)
    # the VMX128 op should show up as "only in B"
    only_b = {m["mnem"] for m in findings["mnemonics_only_in_b"]}
    print("mnemonics only on X360 side:", only_b)
    assert "vmrghw" in only_b, "expected Xenon-only VMX op to be flagged"
    assert (0x10300, 0x82010300) in pairs, "call-graph propagation failed"
    assert pairs[(0x10300, 0x82010300)].method == "callgraph"

    print("\n--- report.md preview ---")
    print(render_markdown(a, b, res, findings)[:900])

    if not ok:
        raise SystemExit("SELFTEST FAILED")
    print("\nSELFTEST PASSED")


if __name__ == "__main__":
    main()
