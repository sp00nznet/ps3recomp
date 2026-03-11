#!/usr/bin/env python3
"""
Auto-generate HLE stub C files for PS3 modules.

Reads the NID database (built-in + optional external JSON) and generates
one .h / .c pair per module, with stub implementations that log
"UNIMPLEMENTED: module::function_name" and return CELL_OK (0).

Usage:
    python generate_stubs.py --output stubs/
    python generate_stubs.py --output stubs/ --modules cellGcmSys cellFs
    python generate_stubs.py --output stubs/ --nid-db extra.json
    python generate_stubs.py --output stubs/ --from-analysis report.json
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nid_database import NIDDatabase, get_default_db

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CELL_OK = 0

HEADER_TEMPLATE = """\
/* Auto-generated HLE stubs for {module} -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for {module} */
{declarations}

/* NID table for runtime registration */
typedef struct {{ uint32_t nid; void* func; const char* name; }} nid_entry_{module_safe};
extern const nid_entry_{module_safe} {module_safe}_nid_table[];
extern const int {module_safe}_nid_table_size;
"""

SOURCE_TEMPLATE = """\
/* Auto-generated HLE stubs for {module} -- do not edit by hand. */

#include "{header_name}"
#include <stdio.h>

{definitions}

/* NID table */
const nid_entry_{module_safe} {module_safe}_nid_table[] = {{
{nid_table_entries}
}};
const int {module_safe}_nid_table_size = sizeof({module_safe}_nid_table) / sizeof({module_safe}_nid_table[0]);
"""

# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------

def _safe_identifier(name: str) -> str:
    """Make a string safe for use as a C identifier."""
    return re.sub(r"[^a-zA-Z0-9_]", "_", name)


def generate_module_stubs(module: str, entries: list[tuple[int, str]],
                          output_dir: str) -> tuple[str, str]:
    """Generate .h and .c files for a module.

    Args:
        module: Module name (e.g. "cellGcmSys").
        entries: List of (nid, function_name) tuples.
        output_dir: Directory to write files to.

    Returns:
        (header_path, source_path) written.
    """
    module_safe = _safe_identifier(module)

    # Declarations
    decl_lines: list[str] = []
    for nid, name in entries:
        safe_name = _safe_identifier(name)
        decl_lines.append(
            f"/* NID 0x{nid:08X} */ int32_t {safe_name}_stub(ppu_context* ctx);"
        )

    # Definitions
    def_lines: list[str] = []
    for nid, name in entries:
        safe_name = _safe_identifier(name)
        def_lines.append(
            f'/* NID 0x{nid:08X} */\n'
            f'int32_t {safe_name}_stub(ppu_context* ctx) {{\n'
            f'    (void)ctx;\n'
            f'    fprintf(stderr, "UNIMPLEMENTED: {module}::{name}\\n");\n'
            f'    return CELL_OK;\n'
            f'}}\n'
        )

    # NID table entries
    nid_entries: list[str] = []
    for nid, name in entries:
        safe_name = _safe_identifier(name)
        nid_entries.append(
            f'    {{ 0x{nid:08X}u, (void*){safe_name}_stub, "{name}" }},'
        )

    header_name = f"{module_safe}_stubs.h"
    source_name = f"{module_safe}_stubs.c"

    header_content = HEADER_TEMPLATE.format(
        module=module,
        module_safe=module_safe,
        declarations="\n".join(decl_lines),
    )

    source_content = SOURCE_TEMPLATE.format(
        module=module,
        module_safe=module_safe,
        header_name=header_name,
        definitions="\n".join(def_lines),
        nid_table_entries="\n".join(nid_entries),
    )

    header_path = os.path.join(output_dir, header_name)
    source_path = os.path.join(output_dir, source_name)

    with open(header_path, "w") as f:
        f.write(header_content)

    with open(source_path, "w") as f:
        f.write(source_content)

    return header_path, source_path


def generate_master_header(modules: list[str], output_dir: str) -> str:
    """Generate a master header that includes all module stubs."""
    lines = [
        "/* Auto-generated master include for all HLE stubs. */",
        "#pragma once",
        "",
    ]
    for module in sorted(modules):
        safe = _safe_identifier(module)
        lines.append(f'#include "{safe}_stubs.h"')

    lines.append("")
    lines.append("/* Master registration function */")
    lines.append("static inline void register_all_stubs(void) {")
    lines.append("    /* Call your runtime's registration for each module: */")
    for module in sorted(modules):
        safe = _safe_identifier(module)
        lines.append(
            f"    /* register_nid_table({safe}_nid_table, {safe}_nid_table_size); */"
        )
    lines.append("}")
    lines.append("")

    path = os.path.join(output_dir, "hle_stubs_all.h")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return path

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Auto-generate HLE stub C files for PS3 modules"
    )
    parser.add_argument("--output", "-o", default="stubs",
                        help="Output directory (default: stubs/)")
    parser.add_argument("--modules", nargs="*",
                        help="Generate stubs only for these modules (default: all)")
    parser.add_argument("--nid-db", metavar="FILE",
                        help="Additional NID database (JSON)")
    parser.add_argument("--from-analysis", metavar="FILE",
                        help="Read required stubs from prx_analyzer JSON report")
    parser.add_argument("--list-modules", action="store_true",
                        help="List available modules and exit")
    args = parser.parse_args()

    db = NIDDatabase()
    db.load_builtins()

    if args.nid_db:
        count = db.load_json(args.nid_db)
        print(f"Loaded {count} extra NID entries", file=sys.stderr)

    # If using analysis report, parse required stubs
    analysis_stubs: dict[str, list[tuple[int, str]]] | None = None
    if args.from_analysis:
        with open(args.from_analysis) as f:
            report = json.load(f)
        required = report.get("required_stubs", {})
        analysis_stubs = {}
        for module, entries in required.items():
            analysis_stubs[module] = [
                (int(e["nid"], 0), e["name"]) for e in entries
            ]

    if args.list_modules:
        print("Available modules in NID database:")
        for mod in db.modules():
            entries = db.module_entries(mod)
            print(f"  {mod}: {len(entries)} functions")
        return

    os.makedirs(args.output, exist_ok=True)

    generated_modules: list[str] = []

    if analysis_stubs:
        # Generate from analysis report
        for module, entries in sorted(analysis_stubs.items()):
            h, c = generate_module_stubs(module, entries, args.output)
            generated_modules.append(module)
            print(f"Generated {os.path.basename(h)} and {os.path.basename(c)}"
                  f"  ({len(entries)} stubs)")

    else:
        # Generate from NID database
        target_modules = args.modules if args.modules else db.modules()

        for module in target_modules:
            entries = db.module_entries(module)
            if not entries:
                print(f"Warning: no entries for module '{module}'", file=sys.stderr)
                continue

            h, c = generate_module_stubs(module, entries, args.output)
            generated_modules.append(module)
            print(f"Generated {os.path.basename(h)} and {os.path.basename(c)}"
                  f"  ({len(entries)} stubs)")

    if generated_modules:
        master = generate_master_header(generated_modules, args.output)
        print(f"Generated master header: {os.path.basename(master)}")
        print(f"\nTotal: {len(generated_modules)} modules, output in {args.output}/")
    else:
        print("No modules to generate.", file=sys.stderr)


if __name__ == "__main__":
    main()
