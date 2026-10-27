"""
ida_export_units.py -- IDAPython script, emits units.json matching schema.py.

The IDA-side counterpart to ExportCompareUnits.java, so IDA and Ghidra are
interchangeable on either platform. IDA's PowerPC processor (with the Xenon
"ppc" / VMX128 plugin) handles the 360 XEX, and plain big-endian PPC handles
the PS3 ELF.

Run headless:
  ida64 -A -S"ida_export_units.py x360 ppc64-xenon out\x360.units.json" default.xex
  ida64 -A -S"ida_export_units.py ps3  ppc64-cell  out\ps3.units.json"  EBOOT.ELF

Or from the IDA UI: File > Script file..., then it uses sane defaults.

Notes:
  * Pure IDAPython (idautils/ida_funcs/ida_bytes/ida_xref) -- no schema.py
    import, because IDA bundles its own Python. The JSON it writes is consumed
    by match_functions.py on the normal interpreter.
"""

import json
import sys

import idaapi
import idautils
import idc
import ida_funcs
import ida_bytes
import ida_nalt
import ida_xref

CONST_MIN = 0x1000  # mirror matcher's CONST_ANCHOR_MIN


def _args():
    # idc.ARGV holds the tokens after the script name in -S"..."
    argv = list(idc.ARGV[1:]) if hasattr(idc, "ARGV") else []
    platform = argv[0] if len(argv) > 0 else "unknown"
    arch = argv[1] if len(argv) > 1 else "unknown"
    out = argv[2] if len(argv) > 2 else (ida_nalt.get_root_filename() + ".units.json")
    return platform, arch, out


def _string_at(ea):
    """Return decoded string at ea if a string is defined there, else None."""
    flags = ida_bytes.get_flags(ea)
    if not ida_bytes.is_strlit(flags):
        return None
    strtype = ida_nalt.get_str_type(ea)
    if strtype is None:
        strtype = ida_nalt.STRTYPE_C
    raw = ida_bytes.get_strlit_contents(ea, -1, strtype)
    if raw is None:
        return None
    try:
        return raw.decode("utf-8", "replace")
    except Exception:
        return raw.decode("latin-1", "replace")


def _func_unit(f):
    start = f.start_ea
    end = f.end_ea
    name = ida_funcs.get_func_name(start)

    mnem = {}
    calls = []
    imports = []
    string_refs = []
    const_refs = []
    insn_count = 0
    is_leaf = True

    for head in idautils.Heads(start, end):
        if not ida_bytes.is_code(ida_bytes.get_flags(head)):
            continue
        insn_count += 1
        m = idc.print_insn_mnem(head).lower()
        if m:
            mnem[m] = mnem.get(m, 0) + 1

        # immediate operands -> const refs
        for op in range(8):
            t = idc.get_operand_type(head, op)
            if t == idc.o_void:
                break
            if t == idc.o_imm:
                v = idc.get_operand_value(head, op) & 0xFFFFFFFFFFFFFFFF
                if v >= CONST_MIN:
                    const_refs.append(v)

        # code refs from this head -> calls
        for xref in idautils.XrefsFrom(head, 0):
            if xref.type in (ida_xref.fl_CN, ida_xref.fl_CF):  # near/far call
                is_leaf = False
                tgt = xref.to
                tf = ida_funcs.get_func(tgt)
                # external/import thunk?
                seg = idc.get_segm_name(tgt) or ""
                if seg.lower() in ("extern", ".idata", "import"):
                    imports.append(idc.get_name(tgt) or ("imp_%08x" % tgt))
                else:
                    calls.append(tgt)

        # data refs from this head -> strings / consts
        for tgt in idautils.DataRefsFrom(head):
            s = _string_at(tgt)
            if s is not None:
                string_refs.append(s)
            else:
                const_refs.append(tgt)

    frame = idc.get_frame_size(start) or 0

    return {
        "addr": "0x%08x" % start,
        "size": end - start,
        "name": name,
        "insn_count": insn_count,
        "is_leaf": is_leaf,
        "stack_size": int(frame),
        "calls": ["0x%08x" % c for c in calls],
        "imports": imports,
        "string_refs": string_refs,
        "const_refs": ["0x%08x" % c for c in const_refs],
        "mnemonic_hist": mnem,
    }


def main():
    # wait for auto-analysis to finish in headless mode
    idaapi.auto_wait()
    platform, arch, out = _args()

    units = []
    for ea in idautils.Functions():
        f = ida_funcs.get_func(ea)
        if not f:
            continue
        if f.flags & ida_funcs.FUNC_THUNK:
            continue
        units.append(_func_unit(f))

    doc = {
        "schema_version": 1,
        "platform": platform,
        "binary": ida_nalt.get_root_filename(),
        "arch": arch,
        "source": "ida",
        "units": units,
    }
    with open(out, "w") as fh:
        json.dump(doc, fh, indent=2)
    print("ida_export_units: wrote %d units -> %s" % (len(units), out))

    # in true headless (-A) mode, exit when done
    if "-A" in idc.ARGV or idaapi.cvar.batch:
        idc.qexit(0)


if __name__ == "__main__":
    main()
