"""Verify exact TC8 aliases against captured RSX fragment programs, without GPU use.

Run with --dll PATH to the current rsx_fp_decompile_buffered_ex DLL. The
capture directory must be the sibling of sdk under the Folklore work root.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re


SDK = Path(__file__).resolve().parents[1]
CAPTURES = SDK.parent
CONTRACT_SOURCE = SDK / "libs/video/rsx_scene_alpha.c"
ALIAS_HEADER = SDK / "libs/video/rsx_scene_alpha_tc8_aliases.h"
# One captured representative per existing full-source alpha contract, in
# rsx_scene_alpha.c contract order. Other copies have identical full HLSL.
REPRESENTATIVES = [
    ("012725", 29), ("012725", 76), ("012725", 33),
    ("012725", 36), ("012725", 37), ("012725", 38),
    ("012725", 39), ("012725", 41), ("012725", 23),
    ("012725", 20), ("012725", 75), ("020002", 105),
    ("020002", 112), ("021115", 120), ("021115", 122),
]
ALIASES = {
    "c0dfeb27d8b4564c76a0c4597a2803e2207e45f087352ac278d7435ab6c0a50c":
        "816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579",
    "f8edbea31132b75c337c8af49016e831fe10930deaffd151a9317f40216acdc8":
        "b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043",
}
RAW_FP_SHA256 = {
    29: "444190866f875fcd03b69f38125bc804cb9e3a5b6d28fd3428e33790b63504dc",
    76: "5ad59d19497802527d67d288a89a05492d6aa9d875ec13abea3d8adc71f1bff9",
    33: "beacf82e5f9e761cf7daf8bf0b058d1a3f12c1d7c15ccd48a750015d97e95b17",
    36: "94478e1847e9041876f61a238751745f9c3ecbe7d88b7bd2a9a8a6d5911f2d76",
    37: "680e019ef52e4e94782680bc465fd8d29d3930a859138aef52f8b9f1ec32d22f",
    38: "9ebf1215b5a54678468d6e22bdf63561a600727112b3dae1dc1b25116dbdc3eb",
    39: "d41580530780a2f440dce64bcd02013e627b4022947cf8185713f8d2d8a792b7",
    41: "a1b12af5bfdafb52e515f5f5ebf09c61ff9c916b85d6ca80094376514a8fc8f3",
    23: "69fa76232463e0c66ccc37f6cf66d5cdadb9874faf52765628788574d6488d05",
    20: "7c8a114b9da2f7ab427c5738a20a40b6f3f88b14299c576256f5fe8d312a6a73",
    75: "5ad59d19497802527d67d288a89a05492d6aa9d875ec13abea3d8adc71f1bff9",
    105: "c0de64a27e093b8307c0b47246dbc40bb2699754807dac03d008b9f05d56489d",
    112: "1d73403ed8cf50df8f4d6cef0544c9a1ead74b0de4a7c3ffdafaf602843c4e0e",
    120: "9c39cc71ec71c6a93618aafd1b1e717ce55f22c340af8d095c96633880596480",
    122: "8ce117366f72e57884a1e295051daaca94411273eadd53fe92ad7cda1f382169",
}
DECLARATION = "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def normalized(data):
    return data.replace(b"\r\n", b"\n")


def contracts():
    source = CONTRACT_SOURCE.read_text()
    entries = re.findall(r'\.hash="([0-9a-f]{64})",\.lf_hash="([0-9a-f]{64})"', source)
    assert len(entries) == len(REPRESENTATIVES) == 15, "contract inventory changed"
    return entries


def header_aliases():
    source = ALIAS_HEADER.read_text()
    entries = re.findall(r'!strcmp\(normalized64hash, "([0-9a-f]{64})"\)\)\s*return "([0-9a-f]{64})";', source)
    assert len(entries) == 2 and dict(entries) == ALIASES, "header alias table drifted"
    assert "return normalized64hash;" in source, "unknown hashes must remain unknown"


def regenerate(dll, shader_dir, pipe, old):
    raw_fp = (shader_dir / f"pipe{pipe}.fp.bin").read_bytes()
    fp_text = (shader_dir / f"pipe{pipe}.fp.txt").read_text()
    ctrl = int(re.search(r"ctrl (0x[0-9a-fA-F]+)", fp_text).group(1), 16)
    cube_mask = sum(1 << int(unit) for unit in re.findall(r"TextureCube rsx_tex(\d+)", old))
    out = ctypes.create_string_buffer(524288)
    count = ctypes.c_uint32()
    rc = dll.rsx_fp_decompile_buffered_ex(raw_fp, len(raw_fp), ctrl,
                                           cube_mask, out, len(out), ctypes.byref(count))
    assert rc > 0, f"pipe{pipe}: fragment decompile failed ({rc})"
    if "/* RSX shader window */" in old:
        rc = dll.rsx_fp_apply_shader_window(out, len(out), 720 | (1 << 12))
        assert rc > 0, f"pipe{pipe}: shader window failed ({rc})"
    if "const float _rsx_alpha_ref" in old:
        rc = dll.rsx_fp_apply_alpha_test_buffered(out, len(out), 0x204)
        assert rc > 0, f"pipe{pipe}: alpha test failed ({rc})"
    return raw_fp, ctrl, cube_mask, count.value, out.value.decode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, help="write checked JSON receipt")
    args = parser.parse_args()
    if os.name == "nt":
        os.add_dll_directory("D:/Tools/msys64/clang64/bin")
    dll = ctypes.CDLL(str(args.dll.resolve()))
    header_aliases()
    results = []
    observed = {}
    for (expected_full, expected_lf), (capture, pipe) in zip(contracts(), REPRESENTATIVES):
        shader_dir = CAPTURES / f"capture-20261019T{capture}Z/shaders"
        path = shader_dir / f"pipe{pipe}.ps.hlsl"
        original_bytes = path.read_bytes()
        assert sha(original_bytes) == expected_full, f"pipe{pipe}: archived full hash changed"
        assert sha(normalized(original_bytes)) == expected_lf, f"pipe{pipe}: archived LF hash changed"
        original = normalized(original_bytes).decode("utf-8")
        raw_fp, ctrl, cube_mask, count, repaired = regenerate(dll, shader_dir, pipe, original)
        assert sha(raw_fp) == RAW_FP_SHA256[pipe], f"pipe{pipe}: raw program changed"
        reversed_source = repaired.replace(DECLARATION, "").replace(
            "input.tc8", "float4(0,0,0,0)").replace("input.tc9", "float4(0,0,0,0)")
        assert reversed_source == original, f"pipe{pipe}: repair changed more than TC8/TC9 source"
        repaired_hash = sha(repaired.encode("utf-8"))
        if pipe in (33, 36):
            assert repaired.count("input.tc8") == 1 and "input.tc9" not in repaired
            assert repaired.count(DECLARATION) == 1
            assert ALIASES.get(repaired_hash) == expected_lf, f"pipe{pipe}: unreviewed new identity"
            observed[repaired_hash] = expected_lf
        else:
            assert repaired_hash == expected_lf, f"pipe{pipe}: unexpected new shader identity"
            assert "input.tc8" not in repaired and "input.tc9" not in repaired
        results.append({"capture": f"20261019T{capture}Z", "pipe": pipe,
                        "raw_fp_sha256": sha(raw_fp), "original_full_sha256": expected_full,
                        "original_lf_sha256": expected_lf, "repaired_lf_sha256": repaired_hash,
                        "ctrl": f"0x{ctrl:x}", "cube_mask": cube_mask,
                        "fp_constants": count, "reverse_diff_exact": True,
                        "alias": repaired_hash in ALIASES})
    assert observed == ALIASES, "not all fixed aliases were reproduced"
    receipt = {"result": "pass", "representatives": len(results),
               "aliases": len(observed), "alpha_lane_review": {
                   "pipe33": "TC8 writes h3.xyz, then RGB/reflection chain. Final t0.a is r0.w from h1.x at the end; its Fresnel inputs use tex1.a, normal tex0, and constants, not TC8.",
                   "pipe36": "TC8 writes h0.xyz, then RGB/reflection chain. Final t0.a is r0.w from h1.x at the end; its Fresnel inputs use tex1.a, normal tex0, and constants, not TC8."
               }, "cases": results}
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(results)} exact captured contracts; {len(observed)} reviewed TC8 aliases")


if __name__ == "__main__":
    main()
