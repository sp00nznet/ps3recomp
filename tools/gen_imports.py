#!/usr/bin/env python3
"""gen_imports.py - emit the imports.json that ppu_lifter --hle-stubs wants.

An ET_EXEC EBOOT has no dynamic section, so prx_analyzer finds nothing; the import
tables hang off sys_proc_prx_param instead, which elf_parser already walks. Each
.lib.stub entry is the address of that import's 0x20-byte trampoline, which is what
the lifter rewrites into ps3_hle_call.

    python gen_imports.py EBOOT.elf -o imports.json
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_parser import ELFFile                            # noqa: E402
from nid_database import get_default_db                  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", default="imports.json")
    args = ap.parse_args()

    elf = ELFFile(args.input)
    elf.load()
    db = get_default_db()

    out, unresolved = [], 0
    for lib in elf.imports:
        for nid, stub in zip(lib.nids, lib.addrs):
            e = {"lib": lib.name_str, "nid": f"0x{nid:08X}", "stub": f"0x{stub:08X}"}
            hit = db.lookup_nid(nid)
            if hit:
                e["name"] = hit[1]
            else:
                unresolved += 1
            out.append(e)

    with open(args.output, "w") as f:
        json.dump(out, f, indent=1)
    named = len(out) - unresolved
    print(f"{len(elf.imports)} libraries, {len(out)} imports, "
          f"{named} named ({named * 100 // max(len(out), 1)}%) -> {args.output}")


if __name__ == "__main__":
    main()
