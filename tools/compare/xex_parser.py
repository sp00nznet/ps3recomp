"""
xex_parser.py -- minimal Xbox 360 XEX2 metadata parser (stdlib only).

This is the 360-side analogue to ps3recomp's elf_parser.py. It parses the XEX2
header and the optional-header directory to recover the metadata we need to
*identify* and *cross-reference* a dump:

  * title id / media id / version   (identify the exact build)
  * entry point + image base        (rebase addresses to match the disassembly)
  * import libraries                 (xboxkrnl.exe, xam.xex, ... -> the 360
                                      equivalent of PS3 NID import stubs)
  * compression / encryption flags   (whether the basefile must be decrypted /
                                      decompressed before it can be disassembled)

It deliberately does NOT implement XEX decryption or LZX decompression -- that
is a large undertaking and already handled well by Ghidra's XEX loader, IDA's
XEX plugin, and `xextool`/`xenia`. For disassembly, load the XEX in Ghidra/IDA
(they unpack the basefile) and run ExportCompareUnits.java / ida_export_units.py.
This parser gives you the metadata layer + a clear "needs unpacking" verdict.

Refs: free60 / xenia xex2 layout. All fields are big-endian.

CLI:
  python xex_parser.py default.xex            # human-readable summary
  python xex_parser.py default.xex --json out.json
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass, field, asdict


# Well-known optional header keys (see xenia xex.h).
OPT_RESOURCE_INFO       = 0x000002FF
OPT_FILE_FORMAT_INFO    = 0x000003FF
OPT_ENTRY_POINT         = 0x00010100
OPT_IMAGE_BASE_ADDRESS  = 0x00010201
OPT_IMPORT_LIBRARIES    = 0x000103FF
OPT_ORIGINAL_PE_NAME    = 0x000183FF
OPT_EXECUTION_INFO      = 0x00040006
OPT_DEFAULT_STACK_SIZE  = 0x00020200

ENCRYPTION = {0: "none", 1: "encrypted"}
COMPRESSION = {0: "none", 1: "basic(raw-blocks)", 2: "compressed(LZX)", 3: "delta"}


class BE:
    """Big-endian byte reader."""

    def __init__(self, data: bytes):
        self.d = data

    def u16(self, off: int) -> int:
        return struct.unpack_from(">H", self.d, off)[0]

    def u32(self, off: int) -> int:
        return struct.unpack_from(">I", self.d, off)[0]


@dataclass
class XexInfo:
    magic: str
    module_flags: int = 0
    pe_data_offset: int = 0
    security_info_offset: int = 0
    optional_header_count: int = 0
    entry_point: int | None = None
    image_base: int | None = None
    default_stack_size: int | None = None
    title_id: int | None = None
    media_id: int | None = None
    version: int | None = None
    base_version: int | None = None
    platform: int | None = None
    disc_number: int | None = None
    disc_count: int | None = None
    encryption: str = "unknown"
    compression: str = "unknown"
    import_libraries: list[str] = field(default_factory=list)
    original_pe_name: str | None = None
    optional_headers: dict[str, str] = field(default_factory=dict)

    @property
    def needs_unpacking(self) -> bool:
        return self.encryption != "none" or self.compression not in ("none", "unknown")

    def to_json(self) -> dict:
        d = asdict(self)
        for k in ("entry_point", "image_base", "title_id", "media_id",
                  "default_stack_size"):
            if d.get(k) is not None:
                d[k] = f"0x{d[k]:08x}"
        d["needs_unpacking"] = self.needs_unpacking
        return d


def _read_opt_blob(r: BE, key: int, value: int) -> bytes | None:
    """Resolve an optional-header entry to its raw bytes (or None if inline)."""
    low = key & 0xFF
    if low in (0x00, 0x01):
        return None  # value is inline; caller interprets `value` directly
    try:
        if low == 0xFF:
            size = r.u32(value)
            return r.d[value:value + size]
        # `low` dwords of data at offset `value`
        return r.d[value:value + low * 4]
    except Exception:
        return None


def _parse_execution_info(blob: bytes, info: XexInfo) -> None:
    if len(blob) < 24:
        return
    r = BE(blob)
    info.media_id = r.u32(0)
    info.version = r.u32(4)
    info.base_version = r.u32(8)
    info.title_id = r.u32(12)
    info.platform = blob[16]
    info.disc_number = blob[18]
    info.disc_count = blob[19]


def _parse_file_format(blob: bytes, info: XexInfo) -> None:
    # blob starts AFTER the variable-length size prefix was used to slice it,
    # but for 0xFF keys the size prefix is included; the struct itself begins
    # with its own u32 size. Be tolerant about offset.
    r = BE(blob)
    # try layout: [u32 size][u16 enc][u16 comp]
    for base in (4, 0):
        try:
            enc = r.u16(base)
            comp = r.u16(base + 2)
            if enc in ENCRYPTION and comp in COMPRESSION:
                info.encryption = ENCRYPTION[enc]
                info.compression = COMPRESSION[comp]
                return
        except Exception:
            continue


def _parse_import_libraries(blob: bytes, info: XexInfo) -> None:
    # blob (after 0xFF size prefix) : [u32 string_table_size][u32 count][names...]
    try:
        r = BE(blob)
        str_table_size = r.u32(0)
        # names live right after the two dwords
        names_region = blob[8:8 + str_table_size]
        parts = names_region.split(b"\x00")
        for p in parts:
            s = p.decode("ascii", "ignore").strip()
            if s and all(32 <= ord(c) < 127 for c in s):
                info.import_libraries.append(s)
    except Exception:
        pass


def parse(path: str) -> XexInfo:
    with open(path, "rb") as fh:
        data = fh.read()
    r = BE(data)
    magic = data[:4].decode("ascii", "replace")
    if magic != "XEX2":
        raise ValueError(f"not a XEX2 file (magic={magic!r}); "
                         f"this parser targets retail XEX2 containers")

    info = XexInfo(
        magic=magic,
        module_flags=r.u32(0x04),
        pe_data_offset=r.u32(0x08),
        security_info_offset=r.u32(0x10),
        optional_header_count=r.u32(0x14),
    )

    off = 0x18
    for _ in range(info.optional_header_count):
        key = r.u32(off)
        value = r.u32(off + 4)
        off += 8
        info.optional_headers[f"0x{key:08x}"] = f"0x{value:08x}"

        if key == OPT_ENTRY_POINT:
            info.entry_point = value
        elif key == OPT_IMAGE_BASE_ADDRESS:
            info.image_base = value
        elif key == OPT_DEFAULT_STACK_SIZE:
            info.default_stack_size = value
        elif key == OPT_EXECUTION_INFO:
            blob = _read_opt_blob(r, key, value)
            if blob:
                _parse_execution_info(blob, info)
        elif key == OPT_FILE_FORMAT_INFO:
            blob = _read_opt_blob(r, key, value)
            if blob:
                _parse_file_format(blob, info)
        elif key == OPT_IMPORT_LIBRARIES:
            blob = _read_opt_blob(r, key, value)
            if blob:
                _parse_import_libraries(blob, info)
        elif key == OPT_ORIGINAL_PE_NAME:
            blob = _read_opt_blob(r, key, value)
            if blob and len(blob) > 4:
                info.original_pe_name = blob[4:].split(b"\x00")[0].decode("ascii", "ignore")

    return info


def main() -> None:
    ap = argparse.ArgumentParser(description="Parse XEX2 header metadata")
    ap.add_argument("xex", help="path to default.xex / *.xex")
    ap.add_argument("--json", help="write metadata JSON here")
    args = ap.parse_args()

    info = parse(args.xex)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(info.to_json(), fh, indent=2)
        print(f"wrote {args.json}")

    print(f"XEX2  title_id={info.title_id and hex(info.title_id)}  "
          f"media_id={info.media_id and hex(info.media_id)}  "
          f"version={info.version}")
    print(f"  entry_point = {info.entry_point and hex(info.entry_point)}")
    print(f"  image_base  = {info.image_base and hex(info.image_base)}")
    print(f"  encryption  = {info.encryption}")
    print(f"  compression = {info.compression}")
    print(f"  imports     = {', '.join(info.import_libraries) or '(none parsed)'}")
    if info.needs_unpacking:
        print("  NOTE: basefile is packed -> load in Ghidra/IDA (XEX loader) or "
              "`xextool -d` before disassembly.")


if __name__ == "__main__":
    main()
