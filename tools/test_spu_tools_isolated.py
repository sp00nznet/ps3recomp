#!/usr/bin/env python3
"""The SPU image tools start under python -I.

Isolated mode leaves the script's own directory off sys.path, so a tool that
imports a sibling module (spu_elf) failed with ModuleNotFoundError before it
parsed its arguments. Each tool runs with -I and --help here and must exit 0.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ["extract_spu_images.py", "spu_program_pipeline.py"]


def main():
    failed = 0
    for name in TOOLS:
        r = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / name), "--help"],
                           capture_output=True, text=True, cwd=str(ROOT.parent))
        ok = r.returncode == 0
        print(f"{'ok  ' if ok else 'FAIL'} python -I tools/{name} --help")
        if not ok:
            print(r.stderr.strip()[-600:])
            failed += 1
    print("PASS" if not failed else "FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
