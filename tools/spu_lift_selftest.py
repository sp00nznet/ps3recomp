"""spu_lift_selftest.py - end-to-end SPU lifter validation against native execution.

The SPU equivalent of lift_selftest.py, for the project's least-validated subsystem. We
proved the SPU *decoder* at 100% vs spu-lv2-objdump; this proves the SPU *lifter*: compile
a corpus of C functions with the real SPU compiler, lift the SPU code with our own
spu_lifter, compile the lifted C on the host against runtime/spu, run it, and check every
result is bit-exact against the same functions compiled and run natively.

Clean-room: the corpus is OUR code; the tool only invokes compilers you already have and
copies no SDK content. Requires the SPU gcc and a host C compiler:

    python spu_lift_selftest.py --spu-gcc .../spu-lv2-gcc.exe --host-cc .../clang.exe
    (or set SPU_GCC / HOST_CC)

Scope: register-only compute (no DMA / channels), so args land in the SPU preferred slot
(gpr[3+i]._u32[0]) and the result in gpr[3]; the channel/branch runtime hooks are stubbed.
Float kept constant-free (no .rodata constant loads, no SPU estimate-based divide).
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from elf_parser import ELFFile

REPO = os.path.dirname(HERE)


def spu_func_addrs(elf_path: str, wanted: set[str]) -> dict[str, int]:
    """name -> local-store address for wanted STT_FUNC symbols in an SPU ELF32 (BE).
    (elf_symbols.py only handles the ELF64 24-byte symbol layout; SPU is ELF32/16-byte.)"""
    e = ELFFile(elf_path); e.load()
    si = next((i for i, s in enumerate(e.section_headers) if s.sh_type == 2), None)
    if si is None:
        raise SystemExit("SPU ELF has no symbol table")
    sd = e.get_section_data(si)
    strd = e.get_section_data(e.section_headers[si].sh_link)
    ent = e.section_headers[si].sh_entsize or 16
    out: dict[str, int] = {}
    for i in range(len(sd) // ent):
        name_o, val, _size, info, _other, _shndx = struct.unpack_from(">IIIBBH", sd, i * ent)
        if (info & 0xF) == 2:                      # STT_FUNC
            z = strd.find(b"\0", name_o)
            nm = strd[name_o:z].decode("latin1")
            if nm in wanted:
                out[nm] = val
    return out
RUNTIME_SPU = os.path.join(REPO, "runtime", "spu")

# (name, signature, source, inputs). ii->i: int(int,int); ff->f: float(float,float)
CORPUS = [
    ("arith", "ii->i", "int arith(int a,int b){ int x=a*3+(b<<2)-(a>>1); x^=(a&b); "
                        "x+=(a>b)?a:b; return x*2-7; }",
     [(5, 3), (-7, 2), (100, -100), (0, 0), (123456, -98765)]),
    ("shifts", "ii->i", "int shifts(int a,int b){ unsigned u=a; int n=b&31; "
                        "return (int)((u<<n)|(u>>((32-n)&31))) + (a>>(n&15)); }",
     [(0x12345678, 5), (-1, 13), (0x80000001, 1), (7, 0)]),
    ("logic", "ii->i", "int logic(int a,int b){ return (a&b)|(a^b)|((~a)&b)|(a<<1); }",
     [(0xF0, 0x0F), (5, 3), (-1, 0), (0x1234, 0x5678)]),
    ("cmp", "ii->i", "int cmp(int a,int b){ int r=0; if(a<b)r|=1; if(a==b)r|=2; if(a>b)r|=4; "
                     "if((unsigned)a<(unsigned)b)r|=8; return r*a-b; }",
     [(1, 2), (2, 1), (3, 3), (-1, 1), (-5, -2)]),
    ("mul", "ii->i", "int mul(int a,int b){ return a*b + (a<<4) - (b>>3); }",
     [(3, 5), (-7, 11), (1000, 1000), (-1, -1)]),
    ("fwork", "ff->f", "float fwork(float a,float b){ return a*b + b - a + a*a; }",
     [(1.5, 2.0), (-3.25, 0.5), (10.0, 4.0)]),
]


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


# SPU runtime hooks the lifted code declares as externs; unused for pure compute.
STUBS = r'''
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
uint8_t* vm_base = 0;
u128 spu_rdch(spu_context* c, uint32_t ch){ (void)c;(void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch){ (void)c;(void)ch; return 1; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v){ (void)c;(void)ch;(void)v; }
void spu_indirect_branch(spu_context* c){ (void)c; fprintf(stderr,"unexpected indirect branch\n"); }
void spu_register_function(uint32_t a, void(*f)(spu_context*)){ (void)a;(void)f; }
void spu_stop(spu_context* c){ (void)c; }
void spu_halt(spu_context* c){ (void)c; }
/* The lifter grew references to these runtime hooks, and the standalone driver
 * did not follow -- the validation has not linked since. Stub them so the test
 * runs again; none affect the arithmetic being checked. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
int g_wws_read_probe = 0;
int g_wws_code_probe = 0;
void yz_lockstep_tick(spu_context* c){ (void)c; }
void spu_task_launch_check(spu_context* c, void* f){ (void)c; (void)f; }
void (*spu_take_interrupt(spu_context* c, void (*tf)(spu_context*)))(spu_context*)
{ (void)c; return tf; }
void spu_img_restore(spu_context* c, int32_t s){ (void)c; (void)s; }
/* Calls return through the trampoline: run it until the callee's `bi $lr`
 * lands back on the return address (the runtime's drain, minus interrupts). */
void spu_drain_call(spu_context* c, uint32_t ret){
    while (g_spu_trampoline_fn) {
        if (g_spu_trampoline_fn == spu_indirect_branch && (c->pc & 0x3FFFF) == (ret & 0x3FFFF))
            { g_spu_trampoline_fn = 0; return; }
        void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(c);
    }
}
'''


def gen_driver(cases_meta):
    lines = [STUBS, ""]
    for name, sig, src, _inp, _addr in cases_meta:
        lines.append(src)                         # native references (host build)
    lines += ["", "int main(){ setbuf(stdout,0); int fails=0,total=0;"]
    for name, sig, _src, inputs, addr in cases_meta:
        fn = f"spu_func_{addr:08X}"
        for inp in inputs:
            ctxset, refargs = [], []
            if sig == "ii->i":
                for i, v in enumerate(inp):
                    ctxset.append(f"ctx.gpr[{3+i}]._u32[0]=(uint32_t)({v});")
                    refargs.append(f"({v})")
                read = "(int)ctx.gpr[3]._u32[0]"
                call = f"(int){name}({','.join(refargs)})"
                fmt = '"%s(%s): lifted=%d ref=%d %s\\n"'
            else:  # ff->f
                for i, v in enumerate(inp):
                    ctxset.append(f"{{ union{{float f;uint32_t u;}} cv; cv.f={v!r}f; "
                                  f"ctx.gpr[{3+i}]._u32[0]=cv.u; }}")
                    refargs.append(f"{v!r}f")
                read = "({ union{float f;uint32_t u;} cv; cv.u=ctx.gpr[3]._u32[0]; cv.f; })"
                call = f"(float){name}({','.join(refargs)})"
                fmt = '"%s(%s): lifted=%g ref=%g %s\\n"'
            argstr = ",".join(str(x) for x in inp)
            lines += [
                "  { spu_context ctx; spu_context_init(&ctx,0);",
                f"    {' '.join(ctxset)}",
                f"    {fn}(&ctx);",
                f"    __typeof__({read}) l={read}, r={call}; total++;",
                "    int ok = (l==r); if(!ok)fails++;",
                f'    printf({fmt}, "{name}", "{argstr}", l, r, ok?"OK":"FAIL"); }}',
            ]
    lines += ['  printf("\\n%s: %d/%d passed\\n", fails?"FAIL":"PASS", total-fails, total);',
              "  return fails; }"]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="End-to-end SPU lifter validation vs native")
    ap.add_argument("--spu-gcc", default=os.environ.get("SPU_GCC"),
                    help="spu-lv2-gcc(.exe) (or SPU_GCC env)")
    ap.add_argument("--host-cc", default=os.environ.get("HOST_CC"),
                    help="host C compiler, e.g. clang (or HOST_CC env)")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    if not args.spu_gcc or not args.host_cc:
        raise SystemExit("need --spu-gcc and --host-cc (or SPU_GCC / HOST_CC). "
                         "Requires the PS3 SDK SPU compiler and a host C compiler.")

    work = tempfile.mkdtemp(prefix="spu_lift_")
    names = [c[0] for c in CORPUS]
    keep = ",".join(f"(void*){n}" for n in names)
    src = os.path.join(work, "corpus.c")
    with open(src, "w") as f:
        f.write("\n".join(c[2] for c in CORPUS) + "\n")
        f.write(f"\nvoid* keep[]={{{keep}}};\nint main(){{ return (int)(long)keep[0]; }}\n")

    elf = os.path.join(work, "corpus.spu.elf")
    sh([args.spu_gcc, "-O2", "-o", elf, src])

    addr = spu_func_addrs(elf, set(names))
    missing = [n for n in names if n not in addr]
    if missing:
        raise SystemExit(f"symbols not found for: {missing} (have {sorted(addr)[:20]})")

    gen = os.path.join(work, "gen")
    sh([sys.executable, os.path.join(HERE, "spu_lifter.py"), "--auto-functions", elf,
        "-o", gen])

    meta = [(n, sig, s, inp, addr[n]) for n, sig, s, inp in CORPUS]
    driver = os.path.join(work, "driver.c")
    open(driver, "w").write(gen_driver(meta))
    exe = os.path.join(work, "spulift.exe")
    sh([args.host_cc, "-std=c11", "-O2", "-w", "-I", gen, "-I", RUNTIME_SPU,
        os.path.join(gen, "spu_recomp.c"), driver, "-o", exe])
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
