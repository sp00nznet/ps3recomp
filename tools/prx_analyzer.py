#!/usr/bin/env python3
"""
PRX module dependency analyser for PS3.

Parses a set of PRX / SPRX files, builds a dependency graph showing which
modules import from which, resolves NID references against the NID database,
and generates a report of required HLE stubs.

Usage:
    python prx_analyzer.py <prx_files...> [--nid-db FILE] [--json] [--graph]
"""

import argparse
import json
import os
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile, ET_SCE_PPURELEXEC
from nid_database import NIDDatabase, get_default_db

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class ModuleInfo:
    """Parsed information for a single PRX module."""
    path: str = ""
    name: str = ""
    exports: list[dict] = field(default_factory=list)  # [{module, nids, names}]
    imports: list[dict] = field(default_factory=list)   # [{module, nids, names}]


@dataclass
class DependencyEdge:
    """An import dependency from one module to another."""
    importer: str = ""
    exporter: str = ""
    nids: list[int] = field(default_factory=list)
    names: list[str] = field(default_factory=list)  # resolved names

# ---------------------------------------------------------------------------
# Analyser
# ---------------------------------------------------------------------------

class PRXAnalyzer:
    """Analyse a set of PRX files for dependencies and NID usage."""

    def __init__(self, nid_db: NIDDatabase | None = None):
        self.nid_db = nid_db or get_default_db()
        self.modules: list[ModuleInfo] = []
        self.edges: list[DependencyEdge] = []
        self.all_import_nids: set[int] = set()
        self.unresolved_nids: set[int] = set()

    def add_file(self, path: str) -> ModuleInfo | None:
        """Parse a PRX/SPRX file and add it to the analysis."""
        try:
            elf = ELFFile(path)
            elf.load()
        except Exception as exc:
            print(f"Warning: could not parse {path}: {exc}", file=sys.stderr)
            return None

        mi = ModuleInfo(path=path)

        # Module name from PRX module info or filename
        if elf.module_info:
            mi.name = elf.module_info.name or os.path.splitext(os.path.basename(path))[0]
        else:
            mi.name = os.path.splitext(os.path.basename(path))[0]

        # Exports
        for exp in elf.exports:
            entry = {
                "module": exp.name_str or mi.name,
                "nids": list(exp.nids),
                "names": [],
            }
            for nid in exp.nids:
                result = self.nid_db.lookup_nid(nid)
                entry["names"].append(result[1] if result else f"nid_0x{nid:08X}")
            mi.exports.append(entry)

        # Imports
        for imp in elf.imports:
            entry = {
                "module": imp.name_str or "unknown",
                "nids": list(imp.nids),
                "names": [],
            }
            for nid in imp.nids:
                self.all_import_nids.add(nid)
                result = self.nid_db.lookup_nid(nid)
                if result:
                    entry["names"].append(result[1])
                else:
                    entry["names"].append(f"nid_0x{nid:08X}")
                    self.unresolved_nids.add(nid)
            mi.imports.append(entry)

        self.modules.append(mi)
        return mi

    def build_dependency_graph(self) -> None:
        """Build edges between modules based on import/export matching."""
        # Build export index: module_name -> set of nids
        export_index: dict[str, set[int]] = {}
        for mod in self.modules:
            for exp in mod.exports:
                export_index.setdefault(exp["module"], set()).update(exp["nids"])

        # Match imports to exports
        self.edges.clear()
        for mod in self.modules:
            for imp in mod.imports:
                imp_module = imp["module"]
                edge = DependencyEdge(
                    importer=mod.name,
                    exporter=imp_module,
                    nids=imp["nids"],
                    names=imp["names"],
                )
                self.edges.append(edge)

    def get_required_stubs(self) -> dict[str, list[dict]]:
        """Return a dict of module -> [{nid, name}] for all imported NIDs
        that are not satisfied by any analysed PRX's exports."""

        exported_nids: set[int] = set()
        for mod in self.modules:
            for exp in mod.exports:
                exported_nids.update(exp["nids"])

        stubs: dict[str, list[dict]] = {}
        for mod in self.modules:
            for imp in mod.imports:
                imp_module = imp["module"]
                for nid, name in zip(imp["nids"], imp["names"]):
                    if nid not in exported_nids:
                        stubs.setdefault(imp_module, []).append({
                            "nid": nid,
                            "name": name,
                        })

        # Deduplicate within each module
        for module in stubs:
            seen: set[int] = set()
            unique = []
            for entry in stubs[module]:
                if entry["nid"] not in seen:
                    seen.add(entry["nid"])
                    unique.append(entry)
            stubs[module] = unique

        return stubs

    # ---- Reporting ----

    def report_text(self) -> str:
        """Generate a human-readable text report."""
        lines: list[str] = []

        lines.append(f"=== PRX Dependency Analysis ===")
        lines.append(f"Modules analysed: {len(self.modules)}")
        lines.append(f"Total imported NIDs: {len(self.all_import_nids)}")
        lines.append(f"Unresolved NIDs: {len(self.unresolved_nids)}")
        lines.append("")

        for mod in self.modules:
            lines.append(f"--- {mod.name} ({os.path.basename(mod.path)}) ---")
            if mod.exports:
                lines.append(f"  Exports:")
                for exp in mod.exports:
                    lines.append(f"    Module: {exp['module']}  ({len(exp['nids'])} NIDs)")
                    for nid, name in zip(exp["nids"], exp["names"]):
                        lines.append(f"      0x{nid:08X}  {name}")
            if mod.imports:
                lines.append(f"  Imports:")
                for imp in mod.imports:
                    lines.append(f"    Module: {imp['module']}  ({len(imp['nids'])} NIDs)")
                    for nid, name in zip(imp["nids"], imp["names"]):
                        lines.append(f"      0x{nid:08X}  {name}")
            lines.append("")

        # Dependency graph
        self.build_dependency_graph()
        lines.append("=== Dependency Graph ===")
        for edge in self.edges:
            lines.append(f"  {edge.importer} -> {edge.exporter}  ({len(edge.nids)} NIDs)")
        lines.append("")

        # Required stubs
        stubs = self.get_required_stubs()
        if stubs:
            lines.append("=== Required HLE Stubs ===")
            for module, entries in sorted(stubs.items()):
                lines.append(f"  [{module}]  ({len(entries)} functions)")
                for entry in entries:
                    lines.append(f"    0x{entry['nid']:08X}  {entry['name']}")
            lines.append("")

        return "\n".join(lines)

    def report_json(self) -> dict:
        """Generate a JSON-serialisable report."""
        self.build_dependency_graph()
        stubs = self.get_required_stubs()

        return {
            "modules": [
                {
                    "name": mod.name,
                    "file": os.path.basename(mod.path),
                    "exports": mod.exports,
                    "imports": mod.imports,
                }
                for mod in self.modules
            ],
            "dependency_graph": [
                {
                    "from": edge.importer,
                    "to": edge.exporter,
                    "nid_count": len(edge.nids),
                    "nids": [f"0x{n:08X}" for n in edge.nids],
                    "names": edge.names,
                }
                for edge in self.edges
            ],
            "required_stubs": {
                module: [{"nid": f"0x{e['nid']:08X}", "name": e["name"]} for e in entries]
                for module, entries in sorted(stubs.items())
            },
            "stats": {
                "modules_analysed": len(self.modules),
                "total_import_nids": len(self.all_import_nids),
                "unresolved_nids": len(self.unresolved_nids),
            },
        }

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="PS3 PRX module dependency analyser"
    )
    parser.add_argument("inputs", nargs="+", help="PRX / SPRX files to analyse")
    parser.add_argument("--nid-db", metavar="FILE",
                        help="Additional NID database (JSON)")
    parser.add_argument("--json", action="store_true",
                        help="Output as JSON")
    parser.add_argument("--graph", action="store_true",
                        help="Show only the dependency graph")
    parser.add_argument("--stubs", action="store_true",
                        help="Show only required HLE stubs")
    args = parser.parse_args()

    db = NIDDatabase()
    db.load_builtins()
    if args.nid_db:
        count = db.load_json(args.nid_db)
        print(f"Loaded {count} extra NID entries", file=sys.stderr)

    analyzer = PRXAnalyzer(nid_db=db)

    for path in args.inputs:
        if os.path.isdir(path):
            for fname in sorted(os.listdir(path)):
                if fname.lower().endswith((".prx", ".sprx", ".self")):
                    analyzer.add_file(os.path.join(path, fname))
        else:
            analyzer.add_file(path)

    if not analyzer.modules:
        print("No modules were successfully parsed.", file=sys.stderr)
        sys.exit(1)

    if args.json:
        report = analyzer.report_json()
        if args.graph:
            print(json.dumps(report["dependency_graph"], indent=2))
        elif args.stubs:
            print(json.dumps(report["required_stubs"], indent=2))
        else:
            print(json.dumps(report, indent=2))
    else:
        if args.graph:
            analyzer.build_dependency_graph()
            for edge in analyzer.edges:
                print(f"{edge.importer} -> {edge.exporter}  ({len(edge.nids)} NIDs)")
        elif args.stubs:
            analyzer.build_dependency_graph()
            stubs = analyzer.get_required_stubs()
            for module, entries in sorted(stubs.items()):
                print(f"[{module}]  ({len(entries)} functions)")
                for entry in entries:
                    print(f"  0x{entry['nid']:08X}  {entry['name']}")
        else:
            print(analyzer.report_text())


if __name__ == "__main__":
    main()
