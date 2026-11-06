#!/usr/bin/env python3
"""Regenerate third_party/smaa/smaa_msl.h from smaa_passes.hlsl + SMAA.hlsl.

Needs glslangValidator and spirv-cross on PATH. Writes the header to stdout:
    python3 third_party/smaa/gen_smaa_msl.py > third_party/smaa/smaa_msl.h
Each entry point goes HLSL -> SPIR-V (glslangValidator -D -V -S <stage> -e
<entry>) -> MSL 2.1 (spirv-cross --msl --msl-version 20100); the preamble and
the SmaaParams block every translation repeats are emitted once.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ENTRIES = [("smaa_edge_vs", "vert"), ("smaa_edge_ps", "frag"),
           ("smaa_weight_vs", "vert"), ("smaa_weight_ps", "frag"),
           ("smaa_blend_vs", "vert"), ("smaa_blend_ps", "frag")]
PREAMBLE = "#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
PARAMS = "struct SmaaParams\n{\n    float4 rtMetrics;\n};\n\n"
HEADER = """/* SMAA 1x (Jimenez et al., MIT -- third_party/smaa/LICENSE.txt) for the Metal
 * present path: third_party/smaa/smaa_passes.hlsl (SMAA.hlsl, SMAA_PRESET_HIGH,
 * luma edges) through glslangValidator -D -V -S <stage> -e <entry> and
 * spirv-cross --msl --msl-version 20100, entry points renamed. Regenerate the
 * same way when SMAA.hlsl or the wrapper changes. */
static const char kSmaaMSL[] =
"""

def c_lines(text):
    out = []
    for line in text.splitlines(keepends=True):
        body = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        out.append('    "%s"\n' % body)
    return "".join(out)

def main():
    src = os.path.join(HERE, "smaa_passes.hlsl")
    msl = "#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n" \
          "struct SmaaParams { float4 rtMetrics; };\n"
    with tempfile.TemporaryDirectory() as tmp:
        for entry, stage in ENTRIES:
            spv = os.path.join(tmp, entry + ".spv")
            subprocess.run(["glslangValidator", "-D", "-V", "-S", stage, "-e", entry, src, "-o", spv],
                           check=True, stdout=subprocess.DEVNULL, cwd=HERE)
            text = subprocess.run(["spirv-cross", "--msl", "--msl-version", "20100", spv,
                                   "--rename-entry-point", entry, entry, stage],
                                  check=True, capture_output=True, text=True).stdout
            if not text.startswith(PREAMBLE):
                sys.exit("unexpected spirv-cross preamble for " + entry)
            text = text[len(PREAMBLE):]
            if text.startswith(PARAMS):
                text = text[len(PARAMS):]
            msl += "// ---- %s\n" % entry + text.rstrip("\n") + "\n"
    sys.stdout.write(HEADER + c_lines(msl) + '    "\\n";\n')

if __name__ == "__main__":
    main()
