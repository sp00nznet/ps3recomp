"""Run the real neutral draw-engine regressions, including immediate vertices."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    if os.name == "nt":
        print("SKIP: neutral C fixture requires the POSIX toolchain; run on macOS/Linux")
        return
    root = Path(__file__).resolve().parents[1]
    sources = ["tests/test_rsx_draw_engine.c", "rsx_draw_engine.c",
               "rsx_dispatch.c", "rsx_vertex_compact.c", "rsx_texture_layout.c",
               "rsx_vp_decompiler.c", "rsx_fp_decompiler.c"]
    with tempfile.TemporaryDirectory(prefix="rsx-immediate-") as directory:
        binary = Path(directory) / "test_rsx_draw_engine"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17",
                        "-Iinclude", "-Ilibs/video", "-o", str(binary),
                        *["libs/video/" + s for s in sources]], cwd=root, check=True)
        subprocess.run([str(binary)], cwd=root, check=True)


if __name__ == "__main__":
    main()
