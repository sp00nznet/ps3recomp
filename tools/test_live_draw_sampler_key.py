#!/usr/bin/env python3
"""The D3D12 live-draw sampler cache key covers every field decode_sampler reads.

libs/video/rsx_live_draw.c keeps one sampler descriptor per sampler_key() value.
decode_sampler reads: min/mag filter (filter bits 16-18 and 24-26), the three
address modes (wrap bits 0-3, 8-11, 16-19) and max/min LOD (control0 bits 7-18
and 19-30). The old key kept wrap bits 0-11 (U and V, never W) and shifted the
21-bit LOD field of control0 left by 18, which keeps 14 bits, so two texture
states that differed only in the W address mode or in min LOD bits above 20
shared one descriptor and the second state was sampled with the first one's
wrap mode or LOD range.

rsx_live_draw.c is D3D12 code and does not build on this host, so the test cuts
sampler_key() out of the source text, compiles it against a stand-in for
rsx_dsp_texture and compares keys of state pairs: pairs decode_sampler tells
apart must get different keys, pairs that differ only in bits it ignores must
keep one key (no needless descriptors).

Run: python tools/test_live_draw_sampler_key.py   (needs a C compiler: CC, cc, clang or gcc)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

BASE = (0x00020000, 0x010101, 0x00000000)          # (filter, wrap, control0)
SPLIT = [                                          # decode_sampler distinguishes these
    ("min LOD bit 25 (control0)",       BASE, (0x00020000, 0x010101, 0x02000000)),
    ("min LOD bit 30 (control0)",       BASE, (0x00020000, 0x010101, 0x40000000)),
    ("W address mode (wrap bits 16-19)", BASE, (0x00020000, 0x030101, 0x00000000)),
    ("max LOD bit 18 (control0)",       BASE, (0x00020000, 0x010101, 0x00040000)),
    ("min filter",                      BASE, (0x00030000, 0x010101, 0x00000000)),
    ("mag filter",                      BASE, (0x01020000, 0x010101, 0x00000000)),
]
SAME = [                                           # decode_sampler cannot
    ("wrap bits 4-7 (unused)",          BASE, (0x00020000, 0x0101F1, 0x00000000)),
    ("control0 bits 0-6 (unused)",      BASE, (0x00020000, 0x010101, 0x0000007F)),
    ("filter bits 0-12 (unused)",       BASE, (0x00020010, 0x010101, 0x00000000)),
]

HARNESS = r'''
#include <stdio.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint64_t u64;
typedef struct { u32 filter, wrap, control0, border_color; } rsx_dsp_texture;
%s
int main(void) {
    static const u32 v[][6] = { %s };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        rsx_dsp_texture a = { v[i][0], v[i][1], v[i][2], 0 }, b = { v[i][3], v[i][4], v[i][5], 0 };
        printf("%%d\n", (u64)sampler_key(&a) == (u64)sampler_key(&b));
    }
    return 0;
}
'''


def sampler_key_source():
    # newline="": the file mixes CRLF and LF lines
    src = open(os.path.join(ROOT, "libs/video/rsx_live_draw.c"), newline="").read().replace("\r\n", "\n")
    m = re.search(r"static u(?:32|64) sampler_key\(const rsx_dsp_texture\* t\)\s*\{", src)
    assert m, "sampler_key not found in libs/video/rsx_live_draw.c"
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    return src[m.start():i]


def compiler():
    return shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")


def merged_flags():
    cc = compiler()
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_live_draw_sampler_key: no C compiler")
        raise SystemExit(0)
    cases = SPLIT + SAME
    vals = ", ".join("{%s}" % ", ".join(hex(x) for x in a + b) for _, a, b in cases)
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        exe = os.path.join(td, "t.exe" if os.name == "nt" else "t")
        with open(c, "w") as f:
            f.write(HARNESS % (sampler_key_source(), vals))
        r = subprocess.run([cc, "-std=c11", "-O0", "-w", "-o", exe, c], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
    return [x == "1" for x in out]


def test_sampler_key():
    merged = merged_flags()
    n = len(SPLIT)
    bad = [name for (name, _, _), m in zip(SPLIT, merged[:n]) if m]
    assert not bad, "states decode_sampler tells apart share one sampler key: " + "; ".join(bad)
    waste = [name for (name, _, _), m in zip(SAME, merged[n:]) if not m]
    assert not waste, "states decode_sampler cannot tell apart get different keys: " + "; ".join(waste)


if __name__ == "__main__":
    test_sampler_key()
    print("PASS: sampler_key separates every field decode_sampler reads and nothing else")
