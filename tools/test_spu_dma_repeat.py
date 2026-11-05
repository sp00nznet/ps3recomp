#!/usr/bin/env python3
"""Drive the identical-transfer watchdog with synthetic GETs and a fake clock."""
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: DMA repeat fixture currently requires a POSIX C compiler")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / "runtime/spu"
common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
          "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
          "spurs_policy_blob_weak.c", "spu_interp.c"]
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}


def run(binary, cwd, *args, extra=None):
    p = subprocess.run([str(binary), *args], env={**env, **(extra or {})},
                       cwd=cwd, capture_output=True, text=True, timeout=30)
    print(p.stdout, end="")
    if p.returncode:
        print(p.stderr)
        raise SystemExit(p.returncode)
    return p.stderr


with tempfile.TemporaryDirectory(prefix="spu-dma-repeat-") as tmp:
    binary = Path(tmp) / "fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
                    *shlex.split(os.environ.get("CFLAGS", "")), "-pthread",
                    "-I" + str(root / "include"), "-I" + str(spu),
                    "-I" + str(root / "runtime/platform"),
                    str(spu / "tests/test_spu_dma_repeat.c"),
                    *[str(spu / name) for name in common],
                    str(root / "runtime/platform/win32_compat.c"), "-lm",
                    "-o", str(binary)], check=True)

    err = run(binary, tmp)
    halts = [l for l in err.splitlines() if "identical transfers" in l]
    assert len(halts) == 1, err
    assert "img=5" in halts[0] and "ea=0x00020000" in halts[0], halts[0]
    assert "[dma-repeat]" not in err, "repeat log printed while unset"

    err = run(binary, tmp, "log", extra={"SPU_DMA_REPEAT_LOG": "4"})
    lines = [l for l in err.splitlines() if l.startswith("[dma-repeat]")]
    assert len(lines) == 1, err
    m = re.search(r"img=(\d+) pc=0x[0-9A-F]{5} ea=0x([0-9A-F]{8}) "
                  r"lsa=0x([0-9A-F]{5}) size=(\d+) repeats=(\d+) us=\d+", lines[0])
    assert m, lines[0]
    assert m.group(1) == "5" and m.group(2) == "00020000", lines[0]
    assert m.group(3) == "01000" and m.group(4) == "16", lines[0]
    assert m.group(5) == "299", lines[0]
    print("PASS: SPU_DMA_REPEAT_LOG names the repeated transfer and its size")
