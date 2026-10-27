#!/usr/bin/env python3
"""Compile production helper/interpreter and actual generated lifted FNMS.
No original program bytes, runtime captures, DMA, channels or guest calls.
Run: python3 tests/test_spu_fnms_zero_extended.py
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / "tools"))
from spu_disasm import spu_decode
from spu_lifter import SPULifter

def main():
    with tempfile.TemporaryDirectory(prefix="spu-fnms-test-") as tmp:
        temp=Path(tmp)
        headers=['#include "spu_helpers.h"']
        for k,dest in enumerate((12,3,5,7)):
            opcode=(13<<28)|(dest<<21)|(5<<14)|(3<<7)|7
            insn=spu_decode(opcode,0x100)
            assert insn.mnemonic=="fnms"
            fn=SPULifter(prefix=f"alias{k}_").lift_function([insn],0x100,0x104)
            headers.append(f"static void {fn.name}(spu_context *ctx) {{")
            headers.extend(fn.body_lines)
            headers.append("}")
        (temp/"fnms_test_lifted.h").write_text("\n".join(headers)+"\n")
        env={k:v for k,v in os.environ.items() if not k.startswith(("SPU_","SPURS_","YDKJ_"))}
        cmd=[os.environ.get("CC","cc"),"-std=gnu17","-O2","-fsanitize=undefined","-fno-sanitize-recover=all","-UNDEBUG"]
        cmd += ["-I"+str(x) for x in (temp,ROOT/"runtime",ROOT/"include",ROOT/"runtime/spu")]
        cmd += [str(ROOT/"tests/test_spu_fnms_zero_extended.c"),str(ROOT/"runtime/spu/spu_interp.c"),"-lm","-o",str(temp/"fixture")]
        subprocess.run(cmd,env=env,check=True,timeout=60)
        return subprocess.run([str(temp/"fixture")],env=env,timeout=15).returncode
if __name__=="__main__":
    raise SystemExit(main())
