"""CPU-only draw-engine decode equivalence and optional authored benchmark."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--draws", type=int, default=400000)
    args = parser.parse_args()
    if args.draws < 1:
        parser.error("--draws must be positive")
    if os.name == "nt":
        print("SKIP: CPU fixture requires the POSIX toolchain; run on macOS/Linux")
        return
    root = Path(__file__).resolve().parents[1]
    sources = ["tests/test_rsx_texture_decode.c", "rsx_draw_engine.c",
               "rsx_dispatch.c", "rsx_vertex_compact.c", "rsx_texture_layout.c",
               "rsx_vp_decompiler.c", "rsx_fp_decompiler.c"]
    with tempfile.TemporaryDirectory(prefix="rsx-texture-decode-") as directory:
        binary = Path(directory) / "test_rsx_texture_decode"
        command = [os.environ.get("CC", "cc"), "-O2", "-std=gnu17",
                   "-Wall", "-Wextra", "-Wno-unused-parameter",
                   "-DRSX_DRAW_ENGINE_TEXTURE_DECODE_TEST", "-Iinclude",
                   "-Ilibs/video", "-o", str(binary),
                   *["libs/video/" + source for source in sources]]
        print("BUILD", " ".join(command), flush=True)
        subprocess.run(command, cwd=root, check=True)
        subprocess.run([str(binary)], cwd=root, check=True)
        if args.benchmark:
            subprocess.run([str(binary), "--bench", str(args.draws)], cwd=root, check=True)


if __name__ == "__main__":
    main()
