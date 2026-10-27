"""lift_selftest.py - end-to-end lifter validation against native execution.

Compiles a small corpus of C functions with the real PS3 PPU compiler, lifts the resulting
PowerPC with our own pipeline (ppu_lifter), compiles the lifted C on the host, runs it, and
checks every result is bit-exact against the SAME functions compiled and run natively. Unlike
test_ppu_lift.py (hand-written per-instruction cases), this exercises whatever the optimizing
compiler actually emits -- register allocation, branch layout, idioms -- end to end.

Clean-room: the corpus is OUR code; the tool only *invokes* compilers you already have and
copies no SDK/toolchain content. Requires the PPU gcc and a host C++ compiler:

    python lift_selftest.py --ppu-gcc .../ppu-lv2-gcc.exe --host-cxx .../clang++.exe
    (or set PPU_GCC / HOST_CXX)

Scope: pure-compute functions (no guest memory / HLE), so the only runtime needed is the
generated ppu_context -- args in gpr[3..]/fpr[1..], result in gpr[3]/fpr[1] per the PPC ABI.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import elf_symbols

# Each case: source, signature, and inputs. Signatures drive how the driver marshals
# arguments into registers and reads the result back.
#   ii->i : int(int,int)   i->i : int(int)   ll->l : int64(int64,int64)
#   ff->f : float(float,float)   dd->d : double(double,double)
CORPUS = [
    ("arith",  "ii->i", "int arith(int a,int b){ int x=a*3+(b<<2)-(a>>1); x^=(a&b); "
                        "x+=(a>b)?a:b; return x*2-7; }",
     [(5, 3), (-7, 2), (100, -100), (0, 0), (123456, -98765), (-2147483647-1, 5)]),
    ("branches", "ii->i", "int branches(int a,int b){ int r=0; if(a<b)r+=1; if(a<=b)r+=2; "
                          "if(a==b)r+=4; if(a>=b)r+=8; if(a>b)r+=16; "
                          "if((unsigned)a<(unsigned)b)r+=32; return r; }",
     [(1, 2), (2, 1), (3, 3), (-1, 1), (-1, -2), (0, -1)]),
    ("shifts", "ii->i", "int shifts(int a,int b){ unsigned u=a; int n=b&31; "
                        "return (int)((u<<n)|(u>>((32-n)&31))) + (a>>(n&15)) - (int)(u>>n); }",
     [(0x12345678, 5), (-1, 13), (0x80000001, 1), (7, 0), (0xdeadbeef - 0x100000000, 20)]),
    ("mul64", "ll->l", "long long mul64(long long a,long long b){ return a*b - (a>>3) + "
                       "(b<<5); }",
     [(3, 5), (-7, 11), (0x100000001, 0x30), (-1, -1), (0x7fffffffffffffff, 2)]),
    # cntlzw only, on a guaranteed-nonzero input (|1): avoids __builtin_clz(0), which is
    # UB on the host but defined (=32) on PPC, and avoids popcount's .rodata lookup table.
    ("clz", "i->i", "int clz(int x){ return __builtin_clz((unsigned)x|1) + (int)((unsigned)x>>28); }",
     [(1,), (0x40000000, ), (0xffff, ), (0x00010000,), (0x7fffffff,)]),
    # Float/double kept constant-free so they stay in registers -- a literal like 0.5f
    # would be loaded from .rodata, which a .text-only lift does not map.
    ("dwork", "dd->d", "double dwork(double a,double b){ return a*b + a/b - b*b + a; }",
     [(1.5, 2.0), (-3.25, 0.5), (100.0, 7.0), (2.0, 1.0)]),
    ("fwork", "ff->f", "float fwork(float a,float b){ return a*b + b - a/b + a*a; }",
     [(1.5, 2.0), (-3.25, 0.5), (10.0, 4.0)]),
]


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


# result register per signature: (kind, is_float)
_RET = {"ii->i": "i32", "i->i": "i32", "ll->l": "i64", "ff->f": "f32", "dd->d": "f64"}


def gen_driver(cases_meta):
    """Emit the C++ driver that runs each lifted func vs its native reference."""
    lines = ['#include "ppu_recomp.h"', "#include <cstdio>", "#include <cstdint>", ""]
    # Runtime stubs: the corpus is register-only, so lifted code never actually reads
    # guest memory or takes an indirect/trampoline path. These definitions exist only
    # to satisfy the linker for symbols the generated .c declares. If a corpus function
    # secretly needed one of these (e.g. a spilled constant), it would return a wrong
    # value and the test would FAIL -- which is the signal we want.
    lines += [
        'extern "C" {',
        "  uint8_t* vm_base = nullptr;",
        "  uint8_t vm_read8(uint64_t){return 0;} uint16_t vm_read16(uint64_t){return 0;}",
        "  uint32_t vm_read32(uint64_t){return 0;} uint64_t vm_read64(uint64_t){return 0;}",
        "  void vm_write8(uint64_t,uint8_t){} void vm_write16(uint64_t,uint16_t){}",
        "  void vm_write32(uint64_t,uint32_t){} void vm_write64(uint64_t,uint64_t){}",
        "  void ps3_indirect_call(ppu_context*){} void ps3_hle_call(unsigned,ppu_context*){}",
        "  uint64_t ppu_timebase_now(){return 0;} void lv2_syscall(ppu_context*){}",
        "  PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = nullptr;",
        "}",
        "",
    ]
    # native references, compiled here on the host for ground truth
    for name, sig, src, _inputs, _addr in cases_meta:
        lines.append(src)
    lines += ["", "int main(){", "    setbuf(stdout,0);", "    int fails=0, total=0;"]
    for name, sig, _src, inputs, addr in cases_meta:
        fn = f"func_{addr:08X}"          # lifter names functions with uppercase hex
        ret = _RET[sig]
        for inp in inputs:
            total_setup = []
            ctxset, refargs = [], []
            if sig in ("ii->i", "i->i", "ll->l"):
                for i, v in enumerate(inp):
                    reg = 3 + i
                    if sig == "ll->l":
                        ctxset.append(f"ctx.gpr[{reg}]=(uint64_t){v}LL;")
                        refargs.append(f"{v}LL")
                    else:
                        ctxset.append(f"ctx.gpr[{reg}]=(uint64_t)(int64_t)({v});")
                        refargs.append(f"({v})")
            else:  # float / double args go in fpr[1..]
                for i, v in enumerate(inp):
                    ctxset.append(f"ctx.fpr[{1+i}]={v!r};")
                    refargs.append(f"{v!r}{'f' if sig=='ff->f' else ''}")
            call = f"{name}({','.join(refargs)})"
            if ret == "i32":
                read, cmp = "(int)(int32_t)ctx.gpr[3]", f"(int){call}"
                fmt = '"%s(%s): lifted=%d ref=%d %s\\n"'
            elif ret == "i64":
                read, cmp = "(long long)ctx.gpr[3]", f"(long long){call}"
                fmt = '"%s(%s): lifted=%lld ref=%lld %s\\n"'
            elif ret == "f32":
                read, cmp = "(float)ctx.fpr[1]", f"(float){call}"
                fmt = '"%s(%s): lifted=%g ref=%g %s\\n"'
            else:
                read, cmp = "(double)ctx.fpr[1]", f"(double){call}"
                fmt = '"%s(%s): lifted=%g ref=%g %s\\n"'
            argstr = ",".join(str(x) for x in inp)
            eq = "l==r" if ret in ("i32", "i64") else "l==r"  # exact match expected
            lines += [
                "    { ppu_context ctx={};",
                f"      {' '.join(ctxset)}",
                f"      {fn}(&ctx);",
                # The lifter fragments a function and chains the pieces through
                # g_trampoline_fn; the runtime drains them after each guest call, so
                # the driver must too, or only the first fragment runs.
                "      while(g_trampoline_fn){ auto _tf=g_trampoline_fn; "
                "g_trampoline_fn=nullptr; _tf(&ctx); }",
                f"      auto l={read}; auto r={cmp}; total++;",
                f"      bool ok=({eq}); if(!ok)fails++;",
                f'      printf({fmt}, "{name}", "{argstr}", l, r, ok?"OK":"FAIL"); }}',
            ]
    lines += ['    printf("\\n%s: %d/%d passed\\n", fails?"FAIL":"PASS", total-fails, total);',
              "    return fails; }"]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="End-to-end lifter validation vs native execution")
    ap.add_argument("--ppu-gcc", default=os.environ.get("PPU_GCC"),
                    help="ppu-lv2-gcc(.exe) (or PPU_GCC env)")
    ap.add_argument("--host-cxx", default=os.environ.get("HOST_CXX"),
                    help="host C++ compiler, e.g. clang++ (or HOST_CXX env)")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    args = ap.parse_args()

    if not args.ppu_gcc or not args.host_cxx:
        raise SystemExit("need --ppu-gcc and --host-cxx (or PPU_GCC / HOST_CXX). "
                         "This test requires the PS3 SDK PPU compiler and a host C++ compiler.")

    work = tempfile.mkdtemp(prefix="lift_selftest_")
    # The corpus is compiled AND LINKED into a real ELF: an unlinked .o leaves inter-block
    # branches as "b ." + relocations, which lift to bogus self-loops. A main() that
    # address-takes every function keeps the optimizer from dropping them.
    names = [c[0] for c in CORPUS]
    keep = ",".join(f"(void*){n}" for n in names)
    src = os.path.join(work, "corpus.c")
    with open(src, "w") as f:
        f.write("\n".join(c[2] for c in CORPUS) + "\n")
        f.write(f"\nvoid* keep[] = {{{keep}}};\nint main(){{ return (int)(long)keep[0]; }}\n")

    elf = os.path.join(work, "corpus.elf")
    sh([args.ppu_gcc, "-O2", "-o", elf, src])

    # code address per function name (STT_FUNC value in .text), and the lifter's
    # function list -- elf_symbols emits exactly the {start,end} shape ppu_lifter wants.
    syms = elf_symbols.load_symbols(elf)
    addr = {s["name"]: int(s["start"], 16) for s in syms}
    missing = [n for n in names if n not in addr]
    if missing:
        raise SystemExit(f"symbols not found for: {missing}")
    fjson = os.path.join(work, "funcs.json")
    json.dump([{"start": s["start"], "end": s["end"]} for s in syms], open(fjson, "w"))

    lifted = os.path.join(work, "lifted")
    sh([sys.executable, os.path.join(HERE, "ppu_lifter.py"), elf,
        "--functions", fjson, "--single-file", "-o", lifted])

    meta = [(name, sig, src_, inputs, addr[name]) for name, sig, src_, inputs in CORPUS]
    driver = os.path.join(work, "driver.cpp")
    open(driver, "w").write(gen_driver(meta))
    exe = os.path.join(work, "lifttest.exe")
    sh([args.host_cxx, "-std=c++17", "-O1", "-w", "-I", lifted,
        os.path.join(lifted, "ppu_recomp.c"), driver, "-o", exe])
    out = subprocess.run([exe], capture_output=True, text=True)
    print(out.stdout, end="")
    if args.keep:
        print(f"[kept] {work}", file=sys.stderr)
    else:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
    return out.returncode


if __name__ == "__main__":
    sys.exit(main())
