#!/usr/bin/env python3
"""Offline regression runner for the DLSS colour transfer helpers.

Extracts dlssSrgbToLinear and dlssLinearToSrgb from the production compute
shader, compiles them unchanged inside a tiny C++17 harness and checks them
against an independent double-precision IEC 61966-2-1 reference. The exact
round trip models the real path: 8-bit UNORM code -> decode -> RGBA16F input
storage -> encode -> RGBA8 UNORM store (round to nearest).

Usage:
  python tests/test_dlss_color_transfer.py --cc PATH_TO_CLANG_CPP --out TEMP_DIR
      [--shader PATH] [--mutate-gamma]

--mutate-gamma replaces the 2.4 exponent with 2.2 in the extracted helpers
and must make this runner fail (exit 1), proving the checks are sensitive.
Exit codes: 0 pass, 1 regression failure, 2 setup/compile/execution failure.
"""
import argparse
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

HELPERS = ("dlssSrgbToLinear", "dlssLinearToSrgb")
DEFAULT_SHADER = Path(__file__).resolve().parent.parent / "libs" / "video" / "rsx_dlss_frame.comp"
FAILURES = []
CHECKS = 0


class SetupError(Exception):
    pass


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def extract(text, name):
    m = re.search(r"\bfloat\s+" + name + r"\s*\([^)]*\)\s*\{", text)
    if not m:
        raise SetupError("helper not found: " + name)
    depth, k = 0, m.end() - 1
    while k < len(text):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[m.start():k + 1]
        k += 1
    raise SetupError("unbalanced braces in " + name)


HARNESS = r"""
#include <cmath>
#include <cstdio>
#include <cstdlib>
using std::pow;
static inline float min(float a, float b) { return a < b ? a : b; }
static inline float max(float a, float b) { return a > b ? a : b; }
%s
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = std::fopen(argv[2], "r");
    if (!in) return 2;
    double v;
    const int decode = argv[1][0] == 'd';
    while (std::fscanf(in, "%%lf", &v) == 1)
        std::printf("%%.9g\n", (double)(decode ? dlssSrgbToLinear((float)v) : dlssLinearToSrgb((float)v)));
    std::fclose(in);
    return 0;
}
"""


def ref_decode(c):
    c = min(max(c, 0.0), 1.0)
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def ref_encode(x):
    x = min(max(x, 0.0), 1.0)
    return x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def fp16(x):
    return struct.unpack("<e", struct.pack("<e", x))[0]


def unorm8(x):
    # Vulkan float -> UNORM: clamp, scale, round to nearest.
    return int(math.floor(min(max(x, 0.0), 1.0) * 255.0 + 0.5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--shader", default=str(DEFAULT_SHADER))
    ap.add_argument("--mutate-gamma", action="store_true")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        raw = Path(a.shader).read_text(encoding="utf-8")
        text = strip_comments(raw)
        helpers = "\n".join(extract(text, h) for h in HELPERS)
        if a.mutate_gamma:
            helpers = helpers.replace("2.4", "2.2")
        src = out / "color_harness.cpp"
        exe = out / "color_harness.exe"
        src.write_text(HARNESS % helpers, encoding="utf-8")
        cc = subprocess.run([a.cc, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                             str(src), "-o", str(exe)], capture_output=True, text=True)
        if cc.returncode:
            raise SetupError("harness compile failed:\n" + cc.stdout + cc.stderr)

        def run(mode, values):
            inp = out / f"{mode}-in.txt"
            inp.write_text("\n".join(repr(float(v)) for v in values) + "\n", encoding="utf-8")
            p = subprocess.run([str(exe), mode, str(inp)], capture_output=True, text=True)
            if p.returncode:
                raise SetupError(f"harness {mode} failed: {p.returncode}")
            got = [float(x) for x in p.stdout.split()]
            if len(got) != len(values):
                raise SetupError("harness output count mismatch")
            return got
    except SetupError as e:
        print("SETUP:", e)
        return 2

    codes = [c / 255.0 for c in range(256)]
    decoded = run("decode", codes)
    stored = [fp16(v) for v in decoded]                 # RGBA16F colorInput
    encoded = run("encode", stored)
    for c, d, e in zip(range(256), decoded, encoded):
        check(unorm8(e) == c, f"8-bit round trip code {c} -> {unorm8(e)}")
        check(abs(d - ref_decode(c / 255.0)) <= 2e-6 + 2e-6 * ref_decode(c / 255.0),
              f"decode({c}/255)={d!r} ref={ref_decode(c / 255.0)!r}")

    sweep = [i / 4096.0 for i in range(4097)]
    dec, enc = run("decode", sweep), run("encode", sweep)
    for x, d, e in zip(sweep, dec, enc):
        check(abs(d - ref_decode(x)) <= 2e-6 + 2e-6 * ref_decode(x), f"decode({x})={d!r}")
        check(abs(e - ref_encode(x)) <= 2e-6 + 2e-6 * ref_encode(x), f"encode({x})={e!r}")
    check(all(b >= a_ for a_, b in zip(dec, dec[1:])), "decode not monotonic")
    check(all(b >= a_ for a_, b in zip(enc, enc[1:])), "encode not monotonic")

    edge = run("decode", [-0.5, 0.0, 1.0, 2.0, 0.5, 0.04045])
    check(edge[0] == 0.0 and edge[1] == 0.0, "decode clamps below 0")
    check(abs(edge[2] - 1.0) < 1e-6 and abs(edge[3] - 1.0) < 1e-6, "decode clamps above 1")
    check(abs(edge[4] - 0.21404114) < 1e-6, f"decode(0.5)={edge[4]!r}")
    check(abs(edge[5] - 0.04045 / 12.92) < 1e-6, "decode threshold continuity")
    edge = run("encode", [-1.0, 0.0, 1.0, 5.0, 0.18, 0.0031308])
    check(edge[0] == 0.0 and edge[1] == 0.0, "encode clamps below 0")
    check(abs(edge[2] - 1.0) < 1e-6 and abs(edge[3] - 1.0) < 1e-6, "encode clamps above 1")
    check(abs(edge[4] - 0.46135613) < 1e-6, f"encode(0.18 mid-gray)={edge[4]!r}")
    check(abs(edge[5] - 0.0031308 * 12.92) < 1e-6, "encode threshold continuity")

    # Placement in the production shader: decode each sample before the
    # two-sample resolve; encode after the clamp; mode 0 is a passthrough.
    flat = re.sub(r"\s+", "", text)
    check(flat.count("dlssDecode(texelFetch(sourceColor,") == 2, "decode must wrap both samples")
    check("dlssEncode(clamp(c,vec4(0),vec4(1)))" in flat, "encode must follow the output clamp")
    check("p.layout_.w==1u?vec4(dlssSrgbToLinear" in flat and "p.layout_.w==1u?vec4(dlssLinearToSrgb" in flat,
          "transfer must be gated on layout_.w==1 with alpha untouched")

    if FAILURES:
        for f in FAILURES[:20]:
            print("FAIL:", f)
        print(f"dlss color transfer: {len(FAILURES)} failures of {CHECKS} checks")
        return 1
    print(f"dlss color transfer: PASS {CHECKS} checks (256-code exact round trip, 4097-point reference sweep)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
