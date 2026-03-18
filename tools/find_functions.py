#!/usr/bin/env python3
"""
PPU function boundary detector for PS3 binaries.

Analyses a PPU ELF or raw binary for function entry/exit points by:
  - Detecting standard PPU prologues (mflr r0; stw/std r0, X(r1); stwu/stdu r1, -Y(r1))
  - Detecting epilogues (lwz/ld r0, X(r1); mtlr r0; blr)
  - Following bl (branch-and-link) targets to discover called functions
  - Building a call graph

Usage:
    python find_functions.py <input_elf_or_bin> [--raw] [--base ADDR] [--json]
                             [--call-graph] [--min-size N]
"""

import argparse
import json
import os
import struct
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ppu_disasm import (Instruction, disassemble_bytes, decode, sign_extend,
                        bits, bit)

# ---------------------------------------------------------------------------
# Instruction classification helpers
# ---------------------------------------------------------------------------

def _is_mflr_r0(insn: Instruction) -> bool:
    return insn.mnemonic == "mflr" and insn.operands.strip() == "r0"


def _is_mtlr_r0(insn: Instruction) -> bool:
    return insn.mnemonic == "mtlr" and insn.operands.strip() == "r0"


def _is_blr(insn: Instruction) -> bool:
    return insn.mnemonic == "blr"


def _is_store_lr(insn: Instruction) -> bool:
    """Check for stw r0, X(r1) or std r0, X(r1) -- saving LR to stack."""
    if insn.mnemonic in ("stw", "std"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r0,") and "(r1)" in ops:
            return True
    return False


def _is_load_lr(insn: Instruction) -> bool:
    """Check for lwz r0, X(r1) or ld r0, X(r1) -- restoring LR from stack."""
    if insn.mnemonic in ("lwz", "ld"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r0,") and "(r1)" in ops:
            return True
    return False


def _is_stack_alloc(insn: Instruction) -> bool:
    """Check for stwu r1, -X(r1) or stdu r1, -X(r1) -- stack frame creation."""
    if insn.mnemonic in ("stwu", "stdu"):
        ops = insn.operands.replace(" ", "")
        if ops.startswith("r1,") and "(r1)" in ops and "-" in ops:
            return True
    return False


def _is_bl(insn: Instruction) -> bool:
    """Check for bl (branch-and-link, non-absolute)."""
    return insn.mnemonic == "bl"


def _bl_target(insn: Instruction) -> int | None:
    """Extract the target address of a bl instruction."""
    if insn.mnemonic != "bl":
        return None
    ops = insn.operands.strip()
    try:
        return int(ops, 16)
    except ValueError:
        return None

# ---------------------------------------------------------------------------
# Function data
# ---------------------------------------------------------------------------

@dataclass
class Function:
    """A detected function with start/end addresses and metadata."""
    start: int = 0
    end: int = 0          # address after last instruction
    stack_size: int = 0   # frame size if detected
    calls: list[int] = field(default_factory=list)  # addresses this function calls
    callers: list[int] = field(default_factory=list)  # addresses that call this function
    has_prologue: bool = False
    has_epilogue: bool = False
    is_leaf: bool = False  # no bl instructions, no LR save

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "stack_size": self.stack_size,
            "has_prologue": self.has_prologue,
            "has_epilogue": self.has_epilogue,
            "is_leaf": self.is_leaf,
            "calls": [f"0x{a:08X}" for a in self.calls],
            "num_callers": len(self.callers),
        }

# ---------------------------------------------------------------------------
# Function finder
# ---------------------------------------------------------------------------

class FunctionFinder:
    """Detect function boundaries in a PPU instruction stream."""

    def __init__(self, instructions: list[Instruction], min_size: int = 8):
        self.instructions = instructions
        self.min_size = min_size  # minimum function size in bytes

        # Build addr -> index lookup
        self._addr_to_idx: dict[int, int] = {}
        for i, insn in enumerate(instructions):
            self._addr_to_idx[insn.addr] = i

        self.functions: dict[int, Function] = {}  # start_addr -> Function

    def find_prologue_functions(self) -> None:
        """Scan for standard function prologues and their matching epilogues."""
        n = len(self.instructions)

        i = 0
        while i < n:
            insn = self.instructions[i]

            # Look for: mflr r0
            if not _is_mflr_r0(insn):
                i += 1
                continue

            # Check next 1-3 instructions for stw/std r0, X(r1)
            found_store = False
            found_stack = False
            stack_size = 0
            scan_end = min(i + 5, n)

            for j in range(i + 1, scan_end):
                nxt = self.instructions[j]
                if _is_store_lr(nxt):
                    found_store = True
                if _is_stack_alloc(nxt):
                    found_stack = True
                    # Extract stack size from operands
                    ops = nxt.operands.replace(" ", "")
                    try:
                        # "r1,-0x50(r1)" -> extract -0x50
                        disp_str = ops.split(",")[1].split("(")[0]
                        stack_size = abs(int(disp_str, 0))
                    except (ValueError, IndexError):
                        stack_size = 0
                    break

            if not found_store:
                i += 1
                continue

            func_start = insn.addr
            func = Function(start=func_start, stack_size=stack_size,
                            has_prologue=True)

            # Scan forward for epilogue (mtlr r0; blr) or just blr
            func_end = None
            for j in range(i + 1, n):
                curr = self.instructions[j]

                # Collect bl targets
                if _is_bl(curr):
                    target = _bl_target(curr)
                    if target is not None:
                        func.calls.append(target)

                # Check for epilogue pattern
                if _is_blr(curr):
                    # Look back 1-3 instructions for mtlr r0
                    for k in range(max(0, j - 3), j):
                        if _is_mtlr_r0(self.instructions[k]):
                            func.has_epilogue = True
                            break

                    func_end = curr.addr + 4
                    break

                # Safety: if we hit another prologue, stop
                if j > i + 2 and _is_mflr_r0(curr):
                    func_end = curr.addr
                    break

            if func_end is None:
                # No blr found, estimate end
                func_end = self.instructions[min(i + 100, n - 1)].addr + 4

            func.end = func_end

            if func.size >= self.min_size:
                self.functions[func_start] = func

            # Advance past this function
            end_idx = self._addr_to_idx.get(func_end, i + 1)
            i = end_idx if end_idx > i else i + 1
            continue

    def find_leaf_functions(self) -> None:
        """Detect leaf functions that don't save LR.

        These are identified as bl targets that don't appear to have a
        standard prologue.  They end at the first blr.
        """
        # Collect all bl targets
        bl_targets: set[int] = set()
        for func in self.functions.values():
            bl_targets.update(func.calls)

        # Also scan all instructions for bl
        for insn in self.instructions:
            if _is_bl(insn):
                target = _bl_target(insn)
                if target is not None:
                    bl_targets.add(target)

        # For each target not already a known function, scan for blr
        for target in bl_targets:
            if target in self.functions:
                continue
            idx = self._addr_to_idx.get(target)
            if idx is None:
                continue

            # Scan for blr (max 200 instructions for a leaf)
            func = Function(start=target, is_leaf=True)
            found_end = False
            for j in range(idx, min(idx + 200, len(self.instructions))):
                curr = self.instructions[j]
                if _is_bl(curr):
                    t = _bl_target(curr)
                    if t is not None:
                        func.calls.append(t)
                        func.is_leaf = False
                if _is_blr(curr):
                    func.end = curr.addr + 4
                    found_end = True
                    break
                # If we hit another known function start, stop
                if curr.addr != target and curr.addr in self.functions:
                    func.end = curr.addr
                    found_end = True
                    break

            if found_end and func.size >= self.min_size:
                self.functions[target] = func

    def build_call_graph(self) -> None:
        """Populate caller lists from call lists."""
        for addr, func in self.functions.items():
            for call_target in func.calls:
                if call_target in self.functions:
                    self.functions[call_target].callers.append(addr)

    def find_branch_target_functions(self) -> None:
        """Third pass: find branch targets that fall outside known function boundaries.

        When the lifter generates code, any branch target that isn't inside a
        known function becomes a missing stub (empty function). This pass scans
        all instructions for branch targets (b, bc, etc.) and creates minimal
        function entries for any that aren't already covered.
        """
        all_targets: set[int] = set()

        for insn in self.instructions:
            mn = insn.mnemonic
            # Collect targets of conditional and unconditional branches
            if mn.startswith("b") and mn not in ("bl", "blr", "bctr", "bctrl", "blrl"):
                ops = insn.operands.strip()
                # Target is usually the last operand
                parts = [p.strip() for p in ops.split(",")]
                for p in parts:
                    if p.startswith("0x"):
                        try:
                            all_targets.add(int(p, 16))
                        except ValueError:
                            pass

        # Filter out targets that are already inside known functions
        new_targets: list[int] = []
        sorted_funcs = sorted(self.functions.values(), key=lambda f: f.start)

        for target in sorted_targets := sorted(all_targets):
            inside = False
            for func in sorted_funcs:
                if func.start <= target < func.end:
                    inside = True
                    break
            if not inside and target not in self.functions:
                new_targets.append(target)

        # Create minimal functions for uncovered targets
        for target in new_targets:
            idx = self._addr_to_idx.get(target)
            if idx is None:
                continue

            func = Function(start=target)
            # Scan forward for blr or next known function
            found_end = False
            for j in range(idx, min(idx + 500, len(self.instructions))):
                curr = self.instructions[j]
                if _is_blr(curr):
                    func.end = curr.addr + 4
                    found_end = True
                    break
                # Stop at known function boundaries
                if curr.addr != target and curr.addr in self.functions:
                    func.end = curr.addr
                    found_end = True
                    break

            if found_end and func.size >= 4:
                self.functions[target] = func

    def run(self) -> list[Function]:
        """Run all detection passes and return sorted function list."""
        self.find_prologue_functions()
        self.find_leaf_functions()
        self.find_branch_target_functions()
        self.build_call_graph()
        return sorted(self.functions.values(), key=lambda f: f.start)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="PPU function boundary detector for PS3 binaries"
    )
    parser.add_argument("input", help="Input ELF file or raw binary")
    parser.add_argument("--raw", action="store_true",
                        help="Treat input as raw binary")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address (for raw binary)")
    parser.add_argument("--little-endian", action="store_true")
    parser.add_argument("--json", action="store_true",
                        help="Output as JSON")
    parser.add_argument("--call-graph", action="store_true",
                        help="Include call graph in output")
    parser.add_argument("--min-size", type=int, default=8,
                        help="Minimum function size in bytes (default: 8)")
    parser.add_argument("--output", "-o", metavar="FILE",
                        help="Write function list to JSON file (for use with ppu_lifter)")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    big_endian = not args.little_endian
    base_addr = args.base

    if args.raw:
        all_insns = disassemble_bytes(file_data, base_addr, big_endian)
    else:
        try:
            from elf_parser import ELFFile, PT_LOAD
            elf = ELFFile(args.input)
            elf.load()
            big_endian = elf.elf_header.big_endian
            all_insns = []
            for ph in elf.program_headers:
                if ph.p_type == PT_LOAD and (ph.p_flags & 1):
                    seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                    all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
            if not all_insns:
                for ph in elf.program_headers:
                    if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                        seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                        all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
                        break
        except Exception as exc:
            print(f"Warning: ELF parse failed ({exc}), treating as raw", file=sys.stderr)
            all_insns = disassemble_bytes(file_data, base_addr, big_endian)

    if not all_insns:
        print("No instructions to analyse.", file=sys.stderr)
        sys.exit(1)

    finder = FunctionFinder(all_insns, min_size=args.min_size)
    functions = finder.run()

    if args.output:
        # Write function list for ppu_lifter
        out_list = [{"start": f"0x{f.start:08X}", "end": f"0x{f.end:08X}"} for f in functions]
        with open(args.output, "w") as f:
            json.dump(out_list, f, indent=2)
        print(f"Wrote {len(functions)} functions to {args.output}")
        return

    if args.json:
        out: dict = {
            "total_functions": len(functions),
            "functions": [f.to_dict() for f in functions],
        }

        if args.call_graph:
            edges: list[dict] = []
            for func in functions:
                for call_target in func.calls:
                    edges.append({
                        "from": f"0x{func.start:08X}",
                        "to": f"0x{call_target:08X}",
                    })
            out["call_graph"] = edges

        print(json.dumps(out, indent=2))

    else:
        print(f"Found {len(functions)} functions:\n")
        print(f"{'Start':>12s}  {'End':>12s}  {'Size':>8s}  {'Stack':>6s}  "
              f"{'Pro':>3s}  {'Epi':>3s}  {'Leaf':>4s}  {'Calls':>5s}  {'Callers':>7s}")
        print("-" * 80)

        for func in functions:
            pro = "Y" if func.has_prologue else "-"
            epi = "Y" if func.has_epilogue else "-"
            leaf = "Y" if func.is_leaf else "-"
            print(f"0x{func.start:08X}  0x{func.end:08X}  {func.size:8d}  "
                  f"{func.stack_size:6d}  {pro:>3s}  {epi:>3s}  {leaf:>4s}  "
                  f"{len(func.calls):5d}  {len(func.callers):7d}")

        if args.call_graph:
            print(f"\nCall graph ({sum(len(f.calls) for f in functions)} edges):")
            for func in functions:
                if func.calls:
                    targets = ", ".join(f"0x{t:08X}" for t in func.calls[:10])
                    more = f" ... (+{len(func.calls) - 10})" if len(func.calls) > 10 else ""
                    print(f"  0x{func.start:08X} -> {targets}{more}")


if __name__ == "__main__":
    main()
