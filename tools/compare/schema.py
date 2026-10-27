"""
Unified function-unit schema for the cross-platform comparison harness.

Both the Xbox 360 (Xenon PPC) side and the PS3 (Cell PPU) side are 64-bit
big-endian PowerPC. We normalize whatever the front-end disassembler produced
(Ghidra, IDA, or ps3recomp's own ppu_disasm/find_functions) into a single
per-function record -- a "Unit" -- so the matcher and report generator never
have to care which platform or tool a record came from.

The on-disk format is one JSON object per binary:

    {
      "platform": "x360" | "ps3",
      "binary":   "default.xex" | "EBOOT.ELF",
      "arch":     "ppc64-xenon" | "ppc64-cell",
      "source":   "ghidra" | "ida" | "ppu_native",
      "units":    [ <Unit>, ... ]
    }

Addresses are serialized as 0x-prefixed hex strings for readability and parsed
back to ints on load. This module is dependency-free (stdlib only) so it can be
imported by the Ghidra/IDA-adjacent tooling as well as the pure-Python matcher.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field, asdict
from typing import Any


SCHEMA_VERSION = 1

# Canonical arch tags.
ARCH_XENON = "ppc64-xenon"   # Xbox 360, PPC970-derived, VMX128
ARCH_CELL = "ppc64-cell"     # PS3 Cell PPU, standard VMX/AltiVec

PLATFORM_X360 = "x360"
PLATFORM_PS3 = "ps3"


def parse_addr(v: Any) -> int:
    """Accept ints or hex/dec strings; return int."""
    if isinstance(v, int):
        return v
    if isinstance(v, str):
        v = v.strip()
        if v.lower().startswith("0x"):
            return int(v, 16)
        return int(v, 10)
    raise TypeError(f"cannot parse address from {v!r}")


def hex_addr(v: int) -> str:
    return f"0x{v:08x}"


_WS = re.compile(r"\s+")


def normalize_string(s: str) -> str:
    """
    Normalize a referenced string literal so the same source string compares
    equal across two builds. Collapses whitespace and trims. We deliberately do
    NOT lowercase -- identifier/path casing is signal.
    """
    return _WS.sub(" ", s).strip()


@dataclass
class Unit:
    """One function/region discovered in a binary."""

    addr: int                                   # function start (effective addr)
    size: int = 0                               # span in bytes
    name: str | None = None                     # symbol/demangled name if known
    insn_count: int = 0
    is_leaf: bool = False                        # makes no calls
    stack_size: int = 0                          # detected frame size, else 0
    calls: list[int] = field(default_factory=list)        # direct call targets
    imports: list[str] = field(default_factory=list)      # imported names/ordinals/NIDs referenced
    string_refs: list[str] = field(default_factory=list)  # normalized string literals referenced
    const_refs: list[int] = field(default_factory=list)   # notable immediates / data addrs
    mnemonic_hist: dict[str, int] = field(default_factory=dict)  # mnemonic -> count

    # ---- serialization -------------------------------------------------
    def to_json(self) -> dict[str, Any]:
        return {
            "addr": hex_addr(self.addr),
            "size": self.size,
            "name": self.name,
            "insn_count": self.insn_count,
            "is_leaf": self.is_leaf,
            "stack_size": self.stack_size,
            "calls": [hex_addr(a) for a in self.calls],
            "imports": list(self.imports),
            "string_refs": list(self.string_refs),
            "const_refs": [hex_addr(c) for c in self.const_refs],
            "mnemonic_hist": dict(self.mnemonic_hist),
        }

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> "Unit":
        return cls(
            addr=parse_addr(d["addr"]),
            size=int(d.get("size", 0)),
            name=d.get("name"),
            insn_count=int(d.get("insn_count", 0)),
            is_leaf=bool(d.get("is_leaf", False)),
            stack_size=int(d.get("stack_size", 0)),
            calls=[parse_addr(a) for a in d.get("calls", [])],
            imports=list(d.get("imports", [])),
            string_refs=[normalize_string(s) for s in d.get("string_refs", [])],
            const_refs=[parse_addr(c) for c in d.get("const_refs", [])],
            mnemonic_hist={str(k): int(v) for k, v in d.get("mnemonic_hist", {}).items()},
        )


@dataclass
class Module:
    """All units from one binary, plus provenance."""

    platform: str
    binary: str
    arch: str
    source: str = "unknown"
    units: list[Unit] = field(default_factory=list)
    schema_version: int = SCHEMA_VERSION

    def by_addr(self) -> dict[int, Unit]:
        return {u.addr: u for u in self.units}

    def to_json(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "platform": self.platform,
            "binary": self.binary,
            "arch": self.arch,
            "source": self.source,
            "units": [u.to_json() for u in self.units],
        }

    @classmethod
    def from_json(cls, d: dict[str, Any]) -> "Module":
        return cls(
            platform=d.get("platform", "unknown"),
            binary=d.get("binary", "unknown"),
            arch=d.get("arch", "unknown"),
            source=d.get("source", "unknown"),
            schema_version=int(d.get("schema_version", SCHEMA_VERSION)),
            units=[Unit.from_json(u) for u in d.get("units", [])],
        )

    def save(self, path: str) -> None:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(self.to_json(), fh, indent=2)

    @classmethod
    def load(cls, path: str) -> "Module":
        with open(path, "r", encoding="utf-8") as fh:
            return cls.from_json(json.load(fh))


__all__ = [
    "SCHEMA_VERSION",
    "ARCH_XENON", "ARCH_CELL", "PLATFORM_X360", "PLATFORM_PS3",
    "Unit", "Module",
    "parse_addr", "hex_addr", "normalize_string",
]
