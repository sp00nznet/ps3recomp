#!/usr/bin/env python3
"""Build the pinned public CACAO shader pack without changing upstream files."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


FORMATS = {
    "g_ClearLoadCounter_LoadCounter": "r32ui",
    "g_EdgeSensitiveBlur_Output": "rg8",
    "g_SSAOOutput": "rg8",
    "g_ApplyOutput": "r8",
    "g_PrepareDepthsOut": "r16f",
    "g_PrepareNormals_NormalOut": "rgba8snorm",
    "g_ImportanceOut": "r8",
    "g_ImportanceAOut": "r8",
    "g_ImportanceBOut": "r8",
    "g_ImportanceBLoadCounter": "r32ui",
    "g_BilateralUpscaleOutput": "r8",
    **{"g_PrepareDepthsAndMips_OutMip" + str(i): "r16f" for i in range(4)},
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def adapt_bindings(content):
    # Modern DXC requires immediate Texture.Load offsets. These two functions
    # load mip0, where adding the integer offset to the coordinate is equivalent.
    for resource in ["g_DepthIn", "g_BilateralUpscaleDepth"]:
        old = (resource + ".Load(int3(coord, 0), offset)").encode()
        new = (resource + ".Load(int3(coord + offset, 0))").encode()
        if content.count(old) != 1:
            raise ValueError("unexpected upstream offset expression: " + resource)
        content = content.replace(old, new)
    # Match the reference implementation's actual VkFormats. DXC defaults to
    # 32-bit storage formats, which are incompatible with its R8/R16 allocations.
    for resource, fmt in FORMATS.items():
        pattern = rb"^(RWTexture[^\n]+\b" + resource.encode() + rb"\s*:[^\n]+)"
        annotation = ('[[vk::image_format("' + fmt + '")]]\n').encode()
        content, count = re.subn(pattern, annotation + rb"\1", content, flags=re.M)
        if count != 1:
            raise ValueError("unexpected upstream storage binding: " + resource)
    return content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", required=True, type=Path)
    parser.add_argument("--validator", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="new output directory; existing output is never overwritten")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "third_party/cacao"
    upstream = json.loads((source / "UPSTREAM.json").read_text())
    for name, expected in upstream["files"].items():
        if digest(source / name) != expected:
            raise ValueError("upstream file changed: " + name)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    generated = output / "PrecompiledShadersSPIRV"
    generated.mkdir()
    compatible = output / "compatible-source"
    compatible.mkdir()
    for name in ["ffx_cacao.hlsl", "ffx_cacao_defines.h", "ffx_cacao_bindings.hlsl"]:
        content = (source / "src" / name).read_bytes()
        if name == "ffx_cacao_bindings.hlsl":
            content = adapt_bindings(content)
        (compatible / name).write_bytes(content)
    receipt = {"upstream": upstream, "passed": False, "steps": [],
               "storage_image_formats": FORMATS, "shaders": []}

    def run(name, command):
        result = subprocess.run([str(s) for s in command], cwd=output,
                                capture_output=True, timeout=90)
        log = output / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        receipt["steps"].append({"name": name, "command": [str(s) for s in command],
                                 "exit_code": result.returncode, "log": log.name})
        (output / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(name + " failed; see " + str(log))
        return result.stdout.decode(errors="replace").strip()

    receipt["compiler_version"] = run("compiler-version", [args.dxc.resolve(), "--version"])
    receipt["compiler_sha256"] = digest(args.dxc.resolve())
    receipt["validator_version"] = run("validator-version", [args.validator.resolve(), "--version"])
    entries = re.findall(
        r"%cauldron_dxc_(16|32)%\s+-Fh\s+PrecompiledShadersSPIRV/(\S+)\s+-Vn\s+(\S+)\s+-E\s+(\S+)\s+ffx_cacao.hlsl",
        (source / "src/build_shaders_spirv.bat").read_text())
    if len(entries) != 66:
        raise ValueError("unexpected upstream shader count")
    for i, (bits, header, symbol, entry) in enumerate(entries):
        stem = Path(header).stem
        binary = output / (stem + ".spv")
        command = [args.dxc.resolve(), "-Wno-conversion", "-spirv", "-T", "cs_6_2",
                   "-HV", "2018", "-fspv-target-env=vulkan1.1",
                   "-fvk-s-shift", "0", "0", "-fvk-b-shift", "10", "0",
                   "-fvk-t-shift", "20", "0", "-fvk-u-shift", "30", "0"]
        if bits == "16":
            command.append("-enable-16bit-types")
        command += ["-E", entry, "-Fo", binary, compatible / "ffx_cacao.hlsl"]
        run(stem, command)
        validator_path = str(binary)
        # WSL can run an installed Windows validator without copying the SDK.
        if str(args.validator).lower().endswith(".exe") and validator_path.startswith("/mnt/"):
            validator_path = validator_path[5].upper() + ":" + validator_path[6:]
        run(stem + "-validate", [args.validator.resolve(), "--target-env", "vulkan1.1", validator_path])
        data = binary.read_bytes()
        if len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
            raise ValueError("invalid SPIR-V byte representation")
        rows = [", ".join("0x%02x" % b for b in data[j:j+16]) for j in range(0, len(data), 16)]
        body = "// Generated from pinned public CACAO sources. See manifest.json.\n"
        body += "alignas(4) const unsigned char " + symbol + "[] = {\n    "
        body += ",\n    ".join(rows) + "\n};\n"
        (generated / header).write_text(body)
        receipt["shaders"].append({"entry": entry, "bits": int(bits), "binary": binary.name,
                                    "sha256": digest(binary), "bytes": len(data),
                                    "header": header, "header_sha256": digest(generated / header)})
        if (i + 1) % 10 == 0:
            print("Compiled and validated %d/66" % (i + 1), flush=True)
    receipt["passed"] = True
    receipt["compatible_source_sha256"] = {p.name: digest(p) for p in compatible.iterdir()}
    (output / "manifest.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("CACAO: 66 validated shaders; runtime testing remains separate.")


if __name__ == "__main__":
    main()
