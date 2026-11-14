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
    # The second build takes a larger texture cache from the command line, as a
    # title whose frames bind more textures does; the header must let it (the
    # test asserts the size it was told to expect).
    for defines in ([], ["-DRSX_DRAW_ENGINE_TEXTURE_CACHE=2048", "-DRSX_TEST_EXPECT_TEXTURE_CACHE=2048"]):
        with tempfile.TemporaryDirectory(prefix="rsx-immediate-") as directory:
            binary = Path(directory) / "test_rsx_draw_engine"
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", *defines,
                            "-Iinclude", "-Ilibs/video", "-o", str(binary),
                            *["libs/video/" + s for s in sources]], cwd=root, check=True)
            subprocess.run([str(binary)], cwd=root, check=True)


if __name__ == "__main__":
    main()
