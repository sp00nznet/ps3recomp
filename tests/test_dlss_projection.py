#!/usr/bin/env python3
"""Offline regression runner for the DLSS inverse homogeneous scale helper.

Extracts dlssInverseHomogeneousScale from the production compute shader,
compiles it unchanged inside a tiny C++17 harness and checks it against
independent double-precision geometry: view-space interpolation with the ray
barycentrics followed by projection.

Usage:
  python tests/test_dlss_projection.py --cc PATH_TO_CLANG_CPP --out TEMP_DIR [--shader PATH]

Exit codes: 0 pass, 1 regression failure, 2 setup/compile/execution failure.
"""
import argparse
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HELPER = "dlssInverseHomogeneousScale"
REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SHADER = REPO_ROOT / "libs" / "video" / "rsx_dlss_frame.comp"
SCALAR_REL_TOL = 1e-5


class SetupError(Exception):
    pass


# ---------------------------------------------------------------- extraction

def strip_comments(text):
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise SetupError("unterminated block comment in shader")
            out.append(" ")
            i = j + 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def extract_helper(source):
    text = strip_comments(source)
    bodies = []
    for m in re.finditer(r"\bfloat\s+" + HELPER + r"\s*\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        if depth:
            raise SetupError("unbalanced parameter list for " + HELPER)
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text) or text[i] != "{":
            continue  # prototype, not a definition
        depth, k = 0, i
        while k < len(text):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if depth:
            raise SetupError("unbalanced braces in " + HELPER)
        bodies.append(text[m.start():k + 1])
    if len(bodies) != 1:
        raise SetupError("expected exactly one definition of %s, found %d" % (HELPER, len(bodies)))
    return bodies[0]


# ---------------------------------------------------------------- geometry

def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def lit(x):
    if math.isnan(x):
        return "NAN_F"
    if math.isinf(x):
        return "INF_F" if x > 0 else "-INF_F"
    return float.hex(f32(x)) + "f"


def clip(p, h, px, py):
    # Captured homogeneous clip x/y/w; z is not an input of the helper.
    return (h * px * p[0], h * py * p[1], h * p[2])


def ref_motion(now_pts, prev_pts, beta, px, py, width, height):
    def project(pts):
        p = [sum(b * v[k] for b, v in zip(beta, pts)) for k in range(3)]
        return px * p[0] / p[2], py * p[1] / p[2]
    nx, ny = project(now_pts)
    qx, qy = project(prev_pts)
    return (qx - nx) * 0.5 * width, (qy - ny) * -0.5 * height


def clip_motion(now, prev, weights, width, height):
    total = sum(weights)
    w = [x / total for x in weights]
    n = [sum(wi * c[k] for wi, c in zip(w, now)) for k in range(3)]
    p = [sum(wi * c[k] for wi, c in zip(w, prev)) for k in range(3)]
    return (p[0] / p[2] - n[0] / n[2]) * 0.5 * width, (p[1] / p[2] - n[1] / n[2]) * -0.5 * height


def make_motion(name, now_pts, prev_pts, h, beta, px, py, size, prev_scale=1.0):
    width, height = size
    ndc = max(max(abs(px * p[0] / p[2]), abs(py * p[1] / p[2])) for p in list(now_pts) + list(prev_pts))
    return {
        "name": name,
        "now": [clip(p, hi, px, py) for p, hi in zip(now_pts, h)],
        "prev": [clip(p, hi * prev_scale, px, py) for p, hi in zip(prev_pts, h)],
        "q": [tuple(p) for p in now_pts],
        "beta": tuple(beta), "px": px, "py": py, "W": width, "H": height,
        "expected": ref_motion(now_pts, prev_pts, beta, px, py, width, height),
        "ndc": ndc, "twin_of": None,
    }


def old_motion(m, now_pts, h, depth, z_scale=None):
    """Pre-fix reconstruction: h=(Cz-A*Cw)/B applied to the normalized capture clip.z."""
    a, b, near, far = depth
    hs = []
    for p, hi, c in zip(now_pts, h, m["now"]):
        hz = hi if z_scale is None else z_scale
        cz = ((a * p[2] + b) - near * p[2]) / (far - near) * hz
        hs.append((cz - a * c[2]) / b)
    try:
        return hs, clip_motion(m["now"], m["prev"], [bb / x for bb, x in zip(m["beta"], hs)], m["W"], m["H"])
    except ZeroDivisionError:
        return hs, (math.inf, math.inf)


def motion_error(got, expected):
    err = max(abs(got[0] - expected[0]), abs(got[1] - expected[1]))
    return err if math.isfinite(err) else math.inf


def depth_params(kind, near, far):
    if kind == "zero_one":
        return (far / (far - near), -near * far / (far - near), near, far)
    if kind == "neg_one_one":
        return ((far + near) / (far - near), -2.0 * far * near / (far - near), near, far)
    return (-near / (far - near), near * far / (far - near), near, far)  # reverse Z


# ---------------------------------------------------------------- fixtures

def geo(name, x, y, z, h, px, py):
    return (name, (h * px * x, h * py * y, h * z, x, y, z, px, py), ("value", 1.0 / h))


def scalar_cases():
    nan, inf, zero = math.nan, math.inf, ("zero",)
    return [
        geo("h=1", 0.4, -0.3, 2.0, 1.0, 1.0, 1.0),
        geo("varied h", -1.5, 0.8, 3.0, 0.5, 1.7320508, 3.0792014),
        geo("varied h large", 2.2, -0.4, 7.5, 3.2, 0.75, 1.3333333),
        geo("negative x strongest", -9.0, 1.0, 2.0, 0.25, 1.1, 0.7),
        # Camera-plane hit: cw == 0 but x/y still carry the scale. The full
        # shader keeps its existing positive captured-w acceptance; this only
        # checks that the helper itself recovers the scale.
        geo("camera plane cw=0 x", 3.0, 1.0, 0.0, 0.5, 1.2, 0.9),
        geo("camera plane cw=0 y", 0.25, -4.0, 0.0, 2.5, 0.8, 1.6),
        geo("tiny h", 3.0, -2.0, 1.0e5, 1.0e-30, 1.0, 1.0),
        geo("huge h", 2.0e-5, 1.0e-5, 1.0e-5, 1.0e30, 1.0, 1.0),
        geo("tiny Q", 1.0e-30, 5.0e-31, 2.0e-30, 2.0, 1.0, 1.0),
        geo("huge Q", 1.0e30, -5.0e29, 3.0e30, 0.5, 1.0, 1.0),
        geo("small ratio below 1e-6", 1.0, 0.5, 4.0, 1.0e8, 1.0, 1.0),
        ("x strongest", (5, 4, 8, 10, 1, 2, 1, 1), ("value", 2.0)),
        ("y strongest", (4, -3, 6, 1, -12, 3, 1, 1), ("value", 4.0)),
        ("z strongest", (2, 8, 3, 1, 2, 9, 1, 1), ("value", 3.0)),
        ("select by |Q| not |U|", (100, 0.1, 1, 1, 2, 3, 1, 1), ("value", 3.0)),
        ("px divides clip x", (6, 1, 1, 6, 1, 1, 2, 0.5), ("value", 2.0)),
        ("py divides clip y", (1, 6, 1, 1, 6, 1, 0.5, 2), ("value", 2.0)),
        ("tie consistent", (1, -1, 1, 2, -2, 2, 1, 1), ("value", 2.0)),
        ("tie x/y", (2, -1, 1, 4, -4, 1, 1, 1), ("oneof", (2.0, 4.0))),
        ("tie x/z", (-1, 1, 2.5, -5, 1, 5, 1, 1), ("oneof", (5.0, 2.0))),
        ("nan cx", (nan, 1, 1, 2, 1, 1, 1, 1), zero),
        ("nan cy unselected", (2, nan, 1, 4, 1, 1, 1, 1), zero),
        ("inf cw", (2, 1, inf, 4, 1, 1, 1, 1), zero),
        ("-inf vx", (2, 1, 1, -inf, 1, 1, 1, 1), zero),
        ("inf vy unselected", (2, 1, 1, 4, inf, 1, 1, 1), zero),
        ("nan vz unselected", (2, 1, 1, 4, 1, nan, 1, 1), zero),
        ("inf px", (2, 1, 1, 4, 1, 1, inf, 1), zero),
        ("nan py", (2, 1, 1, 4, 1, 1, 1, nan), zero),
        ("zero px", (2, 1, 1, 4, 1, 1, 0.0, 1), zero),
        ("negative zero px", (2, 1, 1, 4, 1, 1, -0.0, 1), zero),
        ("zero py unselected", (2, 1, 1, 4, 1, 1, 1, 0.0), zero),
        ("all zero Q", (2, 1, 1, 0, 0, 0, 1, 1), zero),
        ("all zero", (0, 0, 0, 0, 0, 0, 1, 1), zero),
        ("zero selected U", (0, 1, 1, 5, 1, 1, 1, 1), zero),
        ("negative zero selected U", (-0.0, 1, 1, 5, 1, 1, 1, 1), zero),
        ("opposite sign selected", (3, 1, 1, -6, 1, 1, 1, 1), zero),
        ("negative h",) + geo("", 0.5, 0.25, 4.0, -0.5, 1.0, 1.0)[1:2] + (zero,),
        ("ratio overflow", (1.0e-30, 1, 1, 3.0e38, 1, 1, 1, 1), zero),
        ("U overflow", (3.0e38, 1, 1, 2, 1, 1, 1.0e-30, 1), zero),
        ("all nan", (nan,) * 8, zero),
    ]


def motion_cases(checks):
    motions = []

    # 1. Old defect reproduction.
    depth = (1.01, -0.1, 0.2, 1.0)
    now = [(-1.0, -1.0, 2.0), (1.0, -1.0, 4.0), (0.0, 1.0, 8.0)]
    prev = [(-2.0, -1.0, 2.0), (1.0, -1.0, 4.0), (0.0, 1.0, 8.0)]
    m = make_motion("old defect", now, prev, (1.0, 1.0, 1.0), (0.2, 0.3, 0.5), 1.0, 1.0, (1280, 720))
    hs, old = old_motion(m, now, (1.0, 1.0, 1.0), depth)
    checks.append(("fixture expected motionX", abs(m["expected"][0] + 22.8571428571) < 1e-8 and abs(m["expected"][1]) < 1e-12))
    checks.append(("fixture old h", all(abs(a - b) < 1e-9 for a, b in zip(hs, (1.2, 1.15, 1.05)))))
    checks.append(("fixture old motion", abs(old[0] + 20.5668662675) < 1e-8))
    checks.append(("fixture old error > 2px", motion_error(old, m["expected"]) > 2.0))
    motions.append(m)

    # 2. Default [0,1] depth, positive per-vertex h, static and moving scenes.
    base = [(-1.5, -0.8, 3.0), (2.2, -0.4, 7.5), (0.3, 1.6, 12.0)]
    deform = [(0.05, -0.02, 0.1), (-0.03, 0.04, -0.2), (0.0, 0.06, 0.3)]
    cam = (0.12, -0.07, 0.25)
    moved = [tuple(p[k] + d[k] + cam[k] for k in range(3)) for p, d in zip(base, deform)]
    h = (0.5, 1.75, 3.2)
    for px, py in ((1.0, 1.0), (1.7320508, 3.0792014), (0.75, 1.3333333)):
        for size in ((1280, 720), (3440, 1440)):
            for beta in ((0.2, 0.3, 0.5), (0.6, 0.25, 0.15)):
                tag = "default px=%g py=%g %dx%d beta=%s" % (px, py, size[0], size[1], beta)
                motions.append(make_motion(tag + " static", base, base, h, beta, px, py, size))
                motions.append(make_motion(tag + " moving", base, moved, h, beta, px, py, size))

    # 3. Seeded non-default ranges, authored h and homogeneous rescaling.
    rng = random.Random(20261020)
    sizes = ((1280, 720), (1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160))
    kinds = ("zero_one", "neg_one_one", "reverse")
    seeded, worst_old = [], 0.0
    for i in range(32):
        near = rng.uniform(0.05, 1.0)
        far = near * rng.uniform(5.0, 1000.0)
        depth = depth_params(kinds[i % 3], near, far)
        hybrid = i % 3 == 2
        now = []
        for _ in range(3):
            z = rng.uniform(near * 1.5, min(far, 200.0))
            now.append((z * rng.uniform(-1.0, 1.0), z * rng.uniform(-0.8, 0.8), z))
        tz = rng.uniform(-0.25, 0.25) * min(p[2] for p in now)
        tx, ty = rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3)
        prev = [(p[0] + 0.03 * p[2] * rng.uniform(-1, 1) + tx,
                 p[1] + 0.03 * p[2] * rng.uniform(-1, 1) + ty,
                 p[2] * (1.0 + rng.uniform(-0.03, 0.03)) + tz) for p in now]
        h = [math.exp(rng.uniform(math.log(0.125), math.log(8.0))) for _ in range(3)]
        raw = [rng.uniform(0.05, 1.0) for _ in range(3)]
        beta = [b / sum(raw) for b in raw]
        px, py = rng.uniform(0.5, 3.0), rng.uniform(0.5, 3.0)
        size = sizes[rng.randrange(len(sizes))]
        name = "seeded %d %s%s" % (i, kinds[i % 3], " hybrid" if hybrid else "")
        m = make_motion(name, now, prev, h, beta, px, py, size)
        worst_old = max(worst_old, motion_error(old_motion(m, now, h, depth, 1.0 if hybrid else None)[1], m["expected"]))
        motions.append(m)
        seeded.append((len(motions) - 1, now, prev, h, beta, px, py, size))
    checks.append(("seeded fixtures expose old method (>2px)", worst_old > 2.0))
    for idx, now, prev, h, beta, px, py, size in seeded[:8]:
        c = math.exp(rng.uniform(math.log(1e-3), math.log(1e3)))
        c2 = math.exp(rng.uniform(math.log(0.25), math.log(4.0)))
        twin = make_motion(motions[idx]["name"] + " rescaled", now, prev, [x * c for x in h], beta, px, py, size, c2)
        twin["twin_of"] = idx
        motions.append(twin)
    return motions


# ---------------------------------------------------------------- harness

PRELUDE = r"""#include <cmath>
#include <cstdio>
#include <limits>
#define NAN_F std::numeric_limits<float>::quiet_NaN()
#define INF_F std::numeric_limits<float>::infinity()
namespace glsl {
using std::abs;
using std::isinf;
using std::isnan;
typedef unsigned int uint;
struct vec3 {
    float x, y, z;
    vec3() : x(0), y(0), z(0) {}
    explicit vec3(float s) : x(s), y(s), z(s) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    float& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline vec3 abs(vec3 v) { return vec3(std::abs(v.x), std::abs(v.y), std::abs(v.z)); }
inline vec3 operator/(vec3 a, vec3 b) { return vec3(a.x / b.x, a.y / b.y, a.z / b.z); }
#define in
#define highp
#define mediump
// ---- extracted from production shader ----
"""

POSTLUDE = r"""
// ---- end of extracted helper ----
#undef in
#undef highp
#undef mediump
}
"""

MAIN = r"""
int main() {
    for (unsigned i = 0; i < sizeof(kScalar) / sizeof(kScalar[0]); ++i) {
        const float* a = kScalar[i];
        float r = glsl::dlssInverseHomogeneousScale(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        std::printf("S %u %a\n", i, (double)r);
    }
    for (unsigned i = 0; i < sizeof(kMotion) / sizeof(kMotion[0]); ++i) {
        const float* now = kMotion[i];
        const float* q = now + 9;
        const float* prev = now + 18;
        const float* beta = now + 27;
        float px = now[30], py = now[31], width = now[32], height = now[33];
        float s[3], w[3], sum = 0.0f;
        for (int v = 0; v < 3; ++v) {
            s[v] = glsl::dlssInverseHomogeneousScale(now[3 * v], now[3 * v + 1], now[3 * v + 2],
                                                     q[3 * v], q[3 * v + 1], q[3 * v + 2], px, py);
            w[v] = beta[v] * s[v];
            sum += w[v];
        }
        float n[3] = {0.0f, 0.0f, 0.0f}, p[3] = {0.0f, 0.0f, 0.0f};
        for (int v = 0; v < 3; ++v)
            for (int k = 0; k < 3; ++k) {
                n[k] += w[v] / sum * now[3 * v + k];
                p[k] += w[v] / sum * prev[3 * v + k];
            }
        float mx = (p[0] / p[2] - n[0] / n[2]) * 0.5f * width;
        float my = (p[1] / p[2] - n[1] / n[2]) * -0.5f * height;
        std::printf("M %u %a %a %a %a %a\n", i, (double)s[0], (double)s[1], (double)s[2], (double)mx, (double)my);
    }
    return 0;
}
"""


def build_source(helper, scalars, motions):
    rows = ["    {" + ", ".join(lit(v) for v in inp) + "}," for _, inp, _ in scalars]
    mrows = []
    for m in motions:
        flat = [v for c in m["now"] for v in c] + [v for c in m["q"] for v in c] + \
               [v for c in m["prev"] for v in c] + list(m["beta"]) + [m["px"], m["py"], m["W"], m["H"]]
        mrows.append("    {" + ", ".join(lit(v) for v in flat) + "},")
    return (PRELUDE + helper + POSTLUDE +
            "static const float kScalar[][8] = {\n" + "\n".join(rows) + "\n};\n" +
            "static const float kMotion[][34] = {\n" + "\n".join(mrows) + "\n};\n" + MAIN)


def parse_value(token):
    t = token.strip().lower()
    if "nan" in t:
        return math.nan
    if "inf" in t:
        return -math.inf if t.startswith("-") else math.inf
    return float.fromhex(t)


def resolve_cc(value):
    p = Path(value)
    if p.is_file():
        return p.resolve()
    found = shutil.which(value)
    if found:
        return Path(found).resolve()
    raise SetupError("compiler not found: %s" % value)


def prepare_out(value):
    out = Path(value).resolve()
    if out.exists() and not out.is_dir():
        raise SetupError("--out is not a directory: %s" % out)
    if out == REPO_ROOT or out in REPO_ROOT.parents:
        raise SetupError("--out must be a dedicated temp directory, not %s" % out)
    out.mkdir(parents=True, exist_ok=True)
    return out


def close(got, want):
    return got > 0 and abs(got - want) <= SCALAR_REL_TOL * abs(want)


def motion_tol(m):
    return 1e-3 + 1e-5 * 0.5 * max(m["W"], m["H"]) * (1.0 + m["ndc"])


def run(args):
    shader = Path(args.shader).resolve()
    if not shader.is_file():
        raise SetupError("shader not found: %s" % shader)
    cc = resolve_cc(args.cc)
    out = prepare_out(args.out)

    checks = []
    scalars = scalar_cases()
    motions = motion_cases(checks)
    failures = ["fixture check failed: " + name for name, ok in checks if not ok]

    helper = extract_helper(shader.read_text(encoding="utf-8", errors="replace"))
    src = out / "dlss_projection_harness.cpp"
    exe = out / ("dlss_projection_harness" + (".exe" if os.name == "nt" else ""))
    src.write_text(build_source(helper, scalars, motions), encoding="utf-8", newline="\n")

    try:
        comp = subprocess.run([str(cc), "-std=c++17", "-O1", "-ffp-contract=off", "-o", str(exe), str(src)],
                              capture_output=True, text=True, timeout=300, cwd=str(out))
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SetupError("compiler invocation failed: %s" % e)
    if comp.returncode != 0:
        raise SetupError("compile failed (%d):\n%s" % (comp.returncode, (comp.stderr or comp.stdout)[-4000:]))
    try:
        proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, cwd=str(out))
    except (OSError, subprocess.TimeoutExpired) as e:
        raise SetupError("harness execution failed: %s" % e)
    if proc.returncode != 0:
        raise SetupError("harness exited with %d:\n%s" % (proc.returncode, proc.stderr[-2000:]))

    sres, mres = {}, {}
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "S":
            sres[int(parts[1])] = parse_value(parts[2])
        elif len(parts) == 7 and parts[0] == "M":
            mres[int(parts[1])] = [parse_value(t) for t in parts[2:]]
    if len(sres) != len(scalars) or len(mres) != len(motions):
        raise SetupError("harness output incomplete: %d/%d scalar, %d/%d motion"
                         % (len(sres), len(scalars), len(mres), len(motions)))

    for i, (name, _, expect) in enumerate(scalars):
        got = sres[i]
        if not math.isfinite(got) or got < 0:
            failures.append("scalar %s: non-finite or negative result %r" % (name, got))
        elif expect[0] == "zero" and got != 0.0:
            failures.append("scalar %s: expected rejection (0), got %r" % (name, got))
        elif expect[0] == "value" and not close(got, expect[1]):
            failures.append("scalar %s: expected %r, got %r" % (name, expect[1], got))
        elif expect[0] == "oneof" and not any(close(got, v) for v in expect[1]):
            failures.append("scalar %s: expected one of %r, got %r" % (name, expect[1], got))

    for i, m in enumerate(motions):
        s0, s1, s2, mx, my = mres[i]
        tol = motion_tol(m)
        ex, ey = m["expected"]
        if not all(math.isfinite(s) and s > 0 for s in (s0, s1, s2)):
            failures.append("motion %s: helper rejected valid vertex scales %r" % (m["name"], (s0, s1, s2)))
        elif not (math.isfinite(mx) and math.isfinite(my)):
            failures.append("motion %s: non-finite motion (%r, %r)" % (m["name"], mx, my))
        elif abs(mx - ex) > tol or abs(my - ey) > tol:
            failures.append("motion %s: got (%.5f, %.5f) expected (%.5f, %.5f) tol %.3g"
                            % (m["name"], mx, my, ex, ey, tol))
        if m["twin_of"] is not None:
            bx, by = mres[m["twin_of"]][3:5]
            if not (abs(mx - bx) <= tol and abs(my - by) <= tol):
                failures.append("motion %s: not invariant to positive homogeneous rescaling" % m["name"])

    if failures:
        for f in failures[:25]:
            print("FAIL " + f)
        print("dlss projection: %d failure(s) across %d scalar and %d motion cases"
              % (len(failures), len(scalars), len(motions)))
        return 1
    print("dlss projection: PASS %d scalar, %d motion, %d fixture checks" % (len(scalars), len(motions), len(checks)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Offline DLSS projection helper regression runner")
    ap.add_argument("--cc", required=True, help="path to clang++ (C++17)")
    ap.add_argument("--out", required=True, help="owned temporary directory for generated source and executable")
    ap.add_argument("--shader", default=str(DEFAULT_SHADER), help="shader containing " + HELPER)
    args = ap.parse_args(argv)
    try:
        return run(args)
    except SetupError as e:
        print("setup error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
