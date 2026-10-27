"""Self-check for dwarf_common + dwarf_abi on a hand-built DWARF2 blob.

No game binary: synthesize a tiny .debug_info/.debug_abbrev describing one base type and
one struct with two members, then assert the abbrev parser, form reader, DIE-tree walk,
and struct extraction all recover the right offsets/sizes. This exercises the load-bearing
logic the three DWARF tools share.

Run: python tools/test_dwarf.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dwarf_common as dw
import dwarf_abi


def uleb(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def build():
    # --- .debug_abbrev ---
    def decl(code, tag, has_children, attrs):
        b = uleb(code) + uleb(tag) + bytes([has_children])
        for at, fm in attrs:
            b += uleb(at) + uleb(fm)
        return b + b"\x00\x00"

    STRING, DATA1, BLOCK1, REF4 = 0x08, 0x0b, 0x0a, 0x13
    abbrev = (
        decl(1, dw.DW_TAG_compile_unit, 1, [(dw.DW_AT_name, STRING)])
        + decl(2, dw.DW_TAG_base_type, 0, [(dw.DW_AT_name, STRING), (dw.DW_AT_byte_size, DATA1)])
        + decl(3, dw.DW_TAG_structure_type, 1, [(dw.DW_AT_name, STRING), (dw.DW_AT_byte_size, DATA1)])
        + decl(4, dw.DW_TAG_member, 0, [(dw.DW_AT_name, STRING),
                                        (dw.DW_AT_data_member_location, BLOCK1),
                                        (dw.DW_AT_type, REF4)])
        + b"\x00"
    )

    # --- .debug_info ---
    # cu_start == 0 and DIE offsets are absolute within .debug_info, so a ref4 must
    # carry the base_type DIE's absolute offset = 11-byte unit prefix (unit_length 4 +
    # version 2 + abbrev_off 4 + addr_size 1) + its position in the body.
    PREFIX = 11
    body = bytearray()
    body += uleb(1) + b"test.c\x00"                       # compile_unit
    base_ref = PREFIX + len(body)                         # base_type DIE absolute offset
    body += uleb(2) + b"u32\x00" + bytes([4])             # base_type u32, size 4
    body += uleb(3) + b"Foo\x00" + bytes([8])             # struct Foo, size 8
    body += uleb(4) + b"a\x00" + bytes([2, 0x23, 0]) + struct.pack(">I", base_ref)  # +0
    body += uleb(4) + b"b\x00" + bytes([2, 0x23, 4]) + struct.pack(">I", base_ref)  # +4
    body += b"\x00"                                       # end struct children
    body += b"\x00"                                       # end CU children

    header = struct.pack(">HIB", 2, 0, 4)                 # version, abbrev_off, addr_size
    unit = header + bytes(body)
    info = struct.pack(">I", len(unit)) + unit            # unit_length prefix
    return info, abbrev


def main() -> int:
    info, abbrev = build()
    dinfo = dw.DwarfInfo(info, abbrev, b"")

    # Raw DIE walk sanity: depths nest struct > member.
    tags = [(tag, depth) for _o, tag, _v, depth in dinfo.dies()]
    assert (dw.DW_TAG_structure_type, 1) in tags, tags
    assert (dw.DW_TAG_member, 2) in tags, tags

    structs, funcs, types = dwarf_abi.extract(dinfo)
    assert "Foo" in structs, structs
    foo = structs["Foo"]
    assert foo["size"] == 8, foo
    offs = [(o, n) for o, n, _t in foo["members"]]
    assert offs == [(0, "a"), (4, "b")], offs

    # Member type references resolve through the base type.
    tref = foo["members"][0][2]
    assert dwarf_abi.type_name(types, tref) == "u32", dwarf_abi.type_name(types, tref)

    print("ok: DWARF abbrev/form/DIE-tree parse and struct extraction pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
