#!/usr/bin/env python3
"""Insert bench_barrier.h barriers into the lifted benchmark kernels.

usage: apply_barriers.py <spu_recomp.c> <bench_cases.barriers>

Every `ctx->gpr[N] = EXPR;` statement in a kernel becomes
`ctx->gpr[N] = bench_opaque(EXPR);` ("reg" kernels) or bench_opaque_v ("vol":
the op reads no register; "mem": followed by BENCH_CLOBBER); every LS store in
a "mem" kernel is followed by BENCH_CLOBBER. Functions not listed (the call
leaf) are left as lifted.
"""
import re
import sys

ASSIGN = re.compile(r"^(\s*)(ctx->gpr\[\d+\]) = ([^;{}]*);\s*$")
STORE = re.compile(r"^(\s*)spu_ls_write128\(.*\);\s*$")
FUNC = re.compile(r"^void (spu_func_[0-9A-F]{8})\(spu_context\* ctx\) \{")


def main():
    src, bar = sys.argv[1], sys.argv[2]
    kinds = dict(l.split() for l in open(bar) if l.strip())
    out, cur, n = [], None, 0
    for ln in open(src).read().split("\n"):
        m = FUNC.match(ln)
        if m:
            cur = kinds.get(m.group(1))
        elif ln.startswith("}"):
            cur = None
        out.append(ln)
        if not cur:
            continue
        a = ASSIGN.match(ln)
        if a:
            fn = "bench_opaque" if cur == "reg" else "bench_opaque_v"
            out[-1] = "%s%s = %s(%s);%s" % (a.group(1), a.group(2), fn, a.group(3),
                                           " BENCH_CLOBBER();" if cur == "mem" else "")
            n += 1
        elif cur == "mem" and STORE.match(ln):
            out.append("%sBENCH_CLOBBER();" % STORE.match(ln).group(1))
            n += 1
    open(src, "w").write("\n".join(out))
    print("barriers: %d inserted" % n)


main()
