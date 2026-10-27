#!/usr/bin/env python3
"""vp_eval.py - evaluate a decompiled RSX vertex program (the live-draw HLSL,
LD_HLSL_DUMP) on the CPU, with the constants and vertices a real draw used
(LD_TRACE_PSO output: [pso-const] / [pso-vdef] / [pso-trace] attrN lines).

It answers "what does this VS actually output for this vertex" without a GPU
debugger -- e.g. why a draw rasterises black or fails its depth test.

    python tools/vp_eval.py hlsl_<key>.vs.txt trace.log [--vertex 0]

Only the HLSL subset rsx_vp_decompiler.c emits is understood.
"""
import argparse, math, re, struct, sys

C = "xyzw"


TRACE_NAN = "--nan" in sys.argv
WATCH = sys.argv[sys.argv.index("--watch") + 1].split(",") if "--watch" in sys.argv else []


class V:
    """A float vector with HLSL swizzle read/write."""
    def __init__(self, *c):
        if len(c) == 1 and isinstance(c[0], (list, tuple)):
            c = c[0]
        self.c = [float(x) for x in c]

    def _b(self, o):  # broadcast operand to a list
        if isinstance(o, V):
            if len(o.c) == 1: return o.c * len(self.c)
            return o.c
        return [float(o)] * len(self.c)

    def __getattr__(self, s):
        if s and all(ch in C for ch in s):
            return V([self.c[C.index(ch) % len(self.c)] if len(self.c) > 1 else self.c[0] for ch in s])
        raise AttributeError(s)

    def __setattr__(self, s, val):
        if s != "c" and s and all(ch in C for ch in s):
            v = val.c if isinstance(val, V) else [float(val)]
            for i, ch in enumerate(s):
                self.c[C.index(ch)] = v[i] if len(v) > 1 else v[0]
        else:
            object.__setattr__(self, s, val)

    def _op(self, o, f):
        b = self._b(o) if isinstance(o, V) or not isinstance(o, V) else o
        a = self.c
        if isinstance(o, V) and len(o.c) > len(a) and len(a) == 1:
            a = a * len(o.c)
        b = o.c if isinstance(o, V) and len(o.c) == len(a) else (o.c * len(a) if isinstance(o, V) else [float(o)] * len(a))
        return V([f(x, y) for x, y in zip(a, b)])

    __add__ = lambda s, o: s._op(o, lambda a, b: a + b)
    __radd__ = __add__
    __sub__ = lambda s, o: s._op(o, lambda a, b: a - b)
    __mul__ = lambda s, o: s._op(o, lambda a, b: a * b)
    __rmul__ = __mul__
    __truediv__ = lambda s, o: s._op(o, lambda a, b: a / b if b else math.copysign(math.inf, a) if a else math.nan)
    __rtruediv__ = lambda s, o: V([float(o) / x if x else math.copysign(math.inf, float(o)) for x in s.c])
    __rsub__ = lambda s, o: V([float(o) - x for x in s.c])
    __lt__ = lambda s, o: s._op(o, lambda a, b: float(a < b))
    __le__ = lambda s, o: s._op(o, lambda a, b: float(a <= b))
    __gt__ = lambda s, o: s._op(o, lambda a, b: float(a > b))
    __ge__ = lambda s, o: s._op(o, lambda a, b: float(a >= b))
    __eq__ = lambda s, o: s._op(o, lambda a, b: float(a == b))
    __ne__ = lambda s, o: s._op(o, lambda a, b: float(a != b))
    __neg__ = lambda s: V([-x for x in s.c])
    __abs__ = lambda s: V([abs(x) for x in s.c])

    def __float__(self): return self.c[0]
    def __repr__(self): return "(" + ", ".join("%.6g" % x for x in self.c) + ")"


def f4(x):
    if isinstance(x, V):
        return V(x.c * 4 if len(x.c) == 1 else (x.c + [0, 0, 0, 1])[:4])
    return V([float(x)] * 4)


def ew(f):
    return lambda x, *r: V([f(a, *[float(y.c[i] if isinstance(y, V) and len(y.c) > i else (y.c[0] if isinstance(y, V) else y)) for y in r]) for i, a in enumerate(x.c)]) if isinstance(x, V) else f(x, *[float(y) for y in r])


def dot(a, b): return V([sum(x * y for x, y in zip(a.c, b.c))])
def _lm(x, y): return 0.0 if x == 0 or y == 0 else x * y   # NV legacy: 0 * anything = 0
def vp_mul(a, b): return V([_lm(x, y) for x, y in zip(f4(a).c, f4(b).c)])
def vp_dot3(a, b): return V([sum(vp_mul(a, b).c[:3])])
def vp_dot4(a, b): return V([sum(vp_mul(a, b).c)])
def float4(*a): return V([float(x.c[0]) if isinstance(x, V) else float(x) for x in a])
def _max(a, b):
    if isinstance(a, V) or isinstance(b, V):
        a = a if isinstance(a, V) else V([a]); return a._op(b, max)
    return max(a, b)
def _min(a, b):
    if isinstance(a, V) or isinstance(b, V):
        a = a if isinstance(a, V) else V([a]); return a._op(b, min)
    return min(a, b)
saturate = ew(lambda a: min(max(a, 0.0), 1.0))
floor = ew(math.floor); frac = ew(lambda a: a - math.floor(a))
exp2 = ew(lambda a: 2.0 ** max(min(a, 127.0), -127.0))
log2 = ew(lambda a: math.log2(a) if a > 0 else -math.inf)
rsqrt = ew(lambda a: 1.0 / math.sqrt(a) if a > 0 else math.inf)
sign = ew(lambda a: (a > 0) - (a < 0)); sin = ew(math.sin); cos = ew(math.cos)
clamp = ew(lambda a, lo, hi: min(max(a, lo), hi))


def translate(expr):
    e = re.sub(r"(\d)u\b", r"\1", expr)                      # 406u -> 406
    e = re.sub(r"\(float4\)\s*\(", "f4(", e)
    e = e.replace("(float4)0", "f4(0)")
    e = re.sub(r"\bmax\(", "_max(", e); e = re.sub(r"\bmin\(", "_min(", e)
    e = re.sub(r"vp_c\[\(([^\]]*)\) & 511\]", r"vp_c[int(\1) & 511]", e)
    e = re.sub(r"\ba([01])\.([xyzw])", r"int(a\1.\2.c[0])", e)
    e = e.replace("&&", " and ").replace("||", " or ")
    # LIT: "..., ((X).x > 0.0) ? exp2(...) : 0.0, 1.0)"
    e = re.sub(r", \((\(.*?\)\.x) > 0\.0\) \? (exp2\(.*\)) : 0\.0, 1\.0\)$",
               r", _lit(\1, \2), 1.0)", e)
    return e


def _lit(x, y):
    return y if float(x) > 0 else 0.0


def run(hlsl, consts, attrs):
    env = dict(f4=f4, dot=dot, float4=float4, _max=_max, _min=_min, saturate=saturate,
               floor=floor, frac=frac, exp2=exp2, log2=log2, rsqrt=rsqrt, sign=sign,
               sin=sin, cos=cos, clamp=clamp, abs=abs, int=int, float=float, _lit=_lit, vp_mul=vp_mul, vp_dot3=vp_dot3, vp_dot4=vp_dot4)
    env["vp_c"] = consts
    env["v"] = [attrs.get(i, V(0, 0, 0, 1)) for i in range(16)]
    env["r"] = [V(0, 0, 0, 0) for _ in range(32)]
    env["o"] = [V(0, 0, 0, 1) for _ in range(16)]
    env["cc"] = [V(0, 0, 0, 0), V(0, 0, 0, 0)]
    env["a0"] = V(0, 0, 0, 0); env["a1"] = V(0, 0, 0, 0)
    body = hlsl.split("int4 a0 = (int4)0; int4 a1 = (int4)0;")[1].split("VSOutput Out;")[0]
    body = body.split("cc[1] = (float4)0;", 1)[-1]
    cur_expr = ''
    for line in body.splitlines():
        s = line.strip()
        if not s or s in ("}",) or s.startswith("/*"): continue
        m = re.match(r"\{ float4 _v = (.*?);( _v = saturate\(_v\);)?$", s)
        if m:
            ex = m.group(1); cur_expr = ex[:90]
            ex = ex[len("(float4)("):-1] if ex.startswith("(float4)(") else ex
            val = eval(translate(ex), env)
            env["_v"] = f4(val) if isinstance(val, V) and len(val.c) < 4 else (val if isinstance(val, V) else f4(val))
            if m.group(2): env["_v"] = saturate(env["_v"])
            if TRACE_NAN and any(x != x or abs(x) == math.inf for x in env["_v"].c):
                print("non-finite:", env["_v"], "<-", s, file=sys.stderr)
            continue
        m = re.match(r"\{ int4 _a = \(int4\)floor\((.*)\); (a[01])\.(\w+) = _a\.\w+; \}$", s)
        if m:
            val = floor(eval(translate(m.group(1)), env))
            setattr(env[m.group(2)], m.group(3), val); continue
        m = re.match(r"if \((cc\[\d\]\.\w) (\S+) 0\.0\) (.*) = _v\.(\w);$", s)
        if m:
            ccv = float(eval(m.group(1), env)); op = m.group(2)
            if {"<": ccv < 0, "==": ccv == 0, "<=": ccv <= 0, ">": ccv > 0, "!=": ccv != 0, ">=": ccv >= 0}[op]:
                exec("%s = _v.%s" % (m.group(3), m.group(4)), env)
                reg = m.group(3).split(".")[0]
                if WATCH and reg in WATCH:
                    print("%-8s %-40s <- [if %s %s 0] %s" % (reg, eval(reg, env), m.group(1), op, cur_expr), file=sys.stderr)
            continue
        m = re.match(r"(\w+\[\d+\]\.\w+) = _v\.\w+;$", s)
        if m:
            exec(s.rstrip(";"), env)
            if WATCH and m.group(1).split(".")[0] in WATCH:
                reg = m.group(1).split(".")[0]
                print("%-8s %-40s <- %s" % (reg, eval(reg, env), cur_expr), file=sys.stderr)
            continue
        print("?? unparsed:", s, file=sys.stderr)
    return env


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hlsl"); ap.add_argument("trace")
    ap.add_argument("--vertex", type=int, default=0)
    ap.add_argument("--nan", action="store_true", help="report the first non-finite results")
    ap.add_argument("--watch", default="", help="comma list of registers to trace, e.g. r[1],cc[0]")
    ap.add_argument("--draw", type=int, default=0, help="which traced draw (0-based)")
    a = ap.parse_args()
    hl = open(a.hlsl, encoding="utf-8", errors="replace").read()
    consts = [V(0, 0, 0, 0) for _ in range(512)]; attrs = {}
    draw = -1
    for line in open(a.trace, encoding="utf-8", errors="replace"):
        if "[pso-trace] attr0 " in line: draw += 1
        if draw != a.draw: continue
        m = re.search(r"\[pso-const\] (\d+) (\w+) (\w+) (\w+) (\w+)", line)
        if m:
            consts[int(m.group(1))] = V([struct.unpack("<f", struct.pack("<I", int(m.group(i), 16)))[0] for i in range(2, 6)])
        m = re.search(r"\[pso-vdef\] (\d+) (\S+) (\S+) (\S+) (\S+)", line)
        if m and int(m.group(1)) not in attrs:
            attrs[int(m.group(1))] = V([float(m.group(i)) for i in range(2, 6)])
        m = re.search(r"\[pso-trace\] attr(\d+) type=2 size=(\d) stride=\d+ loc=\d off=0x\w+: (.*)", line)
        if m and int(m.group(2)) > 0:
            verts = [x.strip() for x in m.group(3).split("|") if x.strip()]
            if a.vertex < len(verts):
                hx = re.sub(r"[^0-9A-Fa-f]", "", verts[a.vertex])[:8 * int(m.group(2))]
                fl = [struct.unpack(">f", bytes.fromhex(hx[i:i + 8]))[0] for i in range(0, len(hx), 8)]
                attrs[int(m.group(1))] = V((fl + [0, 0, 0, 1][len(fl):])[:4])
    env = run(hl, consts, attrs)
    print("inputs:", {k: attrs[k] for k in sorted(attrs) if k in (0, 2, 3, 8)})
    for i, name in ((0, "HPOS"), (1, "COL0"), (2, "COL1"), (7, "TEX0")):
        print("o[%d] %s = %s" % (i, name, env["o"][i]))
    p = env["o"][0].c
    if p[3]: print("ndc = (%.4g, %.4g, %.4g)" % (p[0] / p[3], p[1] / p[3], p[2] / p[3]))


if __name__ == "__main__":
    main()
