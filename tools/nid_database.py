#!/usr/bin/env python3
"""
PS3 NID (Name ID) resolution database.

NIDs are 32-bit identifiers used by the PS3 LV2 kernel to reference exported
and imported functions/variables in PRX modules.  A NID is computed as the
first 4 bytes (big-endian) of SHA-1(function_name + suffix).

This module provides:
  - NID computation from a function name
  - A built-in database of ~200 common function NIDs from major PS3 modules
  - Loading/saving of external NID databases (JSON)
  - CLI for lookup by NID or name, and batch resolution

Usage:
    python nid_database.py --lookup 0xAB8B4DA4
    python nid_database.py --name cellGcmInit
    python nid_database.py --batch nids.txt
    python nid_database.py --dump
    python nid_database.py --load external_db.json --lookup 0x12345678
"""

import argparse
import hashlib
import json
import os
import struct
import sys

# ---------------------------------------------------------------------------
# NID computation
# ---------------------------------------------------------------------------

# The suffix that Sony appends before hashing.
# This is the well-known suffix used for PS3 NID generation.
NID_SUFFIX = bytes([
    0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x01,
    0xC0, 0xA8, 0x43, 0x09,
])


def compute_nid(name: str, suffix: bytes = NID_SUFFIX) -> int:
    """Compute the PS3 NID for *name*.

    Returns the NID as a 32-bit unsigned integer.
    """
    h = hashlib.sha1(name.encode("utf-8") + suffix)
    digest = h.digest()
    # NID = first 4 bytes, big-endian
    return struct.unpack(">I", digest[:4])[0]

# ---------------------------------------------------------------------------
# Built-in database
# ---------------------------------------------------------------------------

# Each entry is  (module_name, function_name).
# We compute the NIDs at import time so the table stays readable.

_BUILTIN_FUNCTIONS: list[tuple[str, str]] = [
    # ---- cellSysutil ----
    ("cellSysutil", "cellSysutilRegisterCallback"),
    ("cellSysutil", "cellSysutilUnregisterCallback"),
    ("cellSysutil", "cellSysutilCheckCallback"),
    ("cellSysutil", "cellSysutilGetSystemParamInt"),
    ("cellSysutil", "cellSysutilGetSystemParamString"),
    ("cellSysutil", "cellSysutilGetBgmPlaybackStatus"),
    ("cellSysutil", "cellSysutilGetBgmPlaybackStatus2"),
    ("cellSysutil", "cellSysutilEnableBgmPlayback"),
    ("cellSysutil", "cellSysutilDisableBgmPlayback"),
    ("cellSysutil", "cellSysutilEnableBgmPlaybackEx"),
    ("cellSysutil", "cellSysutilDisableBgmPlaybackEx"),
    ("cellSysutil", "cellVideoOutGetState"),
    ("cellSysutil", "cellVideoOutGetResolution"),
    ("cellSysutil", "cellVideoOutConfigure"),
    ("cellSysutil", "cellVideoOutGetConfiguration"),
    ("cellSysutil", "cellVideoOutGetDeviceInfo"),
    ("cellSysutil", "cellVideoOutGetNumberOfDevice"),
    ("cellSysutil", "cellVideoOutGetResolutionAvailability"),
    ("cellSysutil", "cellAudioOutGetState"),
    ("cellSysutil", "cellAudioOutConfigure"),
    ("cellSysutil", "cellAudioOutGetSoundAvailability"),
    ("cellSysutil", "cellAudioOutGetDeviceInfo"),

    # ---- cellGcmSys ----
    ("cellGcmSys", "cellGcmInit"),
    ("cellGcmSys", "cellGcmSetFlipMode"),
    ("cellGcmSys", "cellGcmGetFlipStatus"),
    ("cellGcmSys", "cellGcmResetFlipStatus"),
    ("cellGcmSys", "cellGcmSetWaitFlip"),
    ("cellGcmSys", "cellGcmSetFlip"),
    ("cellGcmSys", "cellGcmGetCurrentField"),
    ("cellGcmSys", "cellGcmGetLabelAddress"),
    ("cellGcmSys", "cellGcmGetControlRegister"),
    ("cellGcmSys", "cellGcmGetConfiguration"),
    ("cellGcmSys", "cellGcmAddressToOffset"),
    ("cellGcmSys", "cellGcmSetDisplayBuffer"),
    ("cellGcmSys", "cellGcmGetDisplayBufferByFlipIndex"),
    ("cellGcmSys", "cellGcmIoOffsetToAddress"),
    ("cellGcmSys", "cellGcmMapMainMemory"),
    ("cellGcmSys", "cellGcmMapEaIoAddress"),
    ("cellGcmSys", "cellGcmMapEaIoAddressWithFlags"),
    ("cellGcmSys", "cellGcmUnmapEaIoAddress"),
    ("cellGcmSys", "cellGcmUnmapIoAddress"),
    ("cellGcmSys", "cellGcmSetTileInfo"),
    ("cellGcmSys", "cellGcmBindTile"),
    ("cellGcmSys", "cellGcmUnbindTile"),
    ("cellGcmSys", "cellGcmSetZcull"),
    ("cellGcmSys", "cellGcmBindZcull"),
    ("cellGcmSys", "cellGcmUnbindZcull"),
    ("cellGcmSys", "cellGcmGetTiledPitchSize"),
    ("cellGcmSys", "cellGcmSetFlipHandler"),
    ("cellGcmSys", "cellGcmSetVBlankHandler"),
    ("cellGcmSys", "cellGcmSetGraphicsHandler"),
    ("cellGcmSys", "cellGcmSetSecondVHandler"),
    ("cellGcmSys", "cellGcmGetCurrentDisplayBufferId"),
    ("cellGcmSys", "cellGcmSetDefaultCommandBuffer"),
    ("cellGcmSys", "cellGcmSetDefaultFifoSize"),
    ("cellGcmSys", "cellGcmGetDefaultCommandWordSize"),
    ("cellGcmSys", "cellGcmGetDefaultSegmentWordSize"),
    ("cellGcmSys", "cellGcmInitDefaultFifoMode"),
    ("cellGcmSys", "cellGcmSetupContextData"),

    # ---- cellFs ----
    ("cellFs", "cellFsOpen"),
    ("cellFs", "cellFsClose"),
    ("cellFs", "cellFsRead"),
    ("cellFs", "cellFsWrite"),
    ("cellFs", "cellFsLseek"),
    ("cellFs", "cellFsFstat"),
    ("cellFs", "cellFsStat"),
    ("cellFs", "cellFsMkdir"),
    ("cellFs", "cellFsRmdir"),
    ("cellFs", "cellFsRename"),
    ("cellFs", "cellFsUnlink"),
    ("cellFs", "cellFsOpendir"),
    ("cellFs", "cellFsClosedir"),
    ("cellFs", "cellFsReaddir"),
    ("cellFs", "cellFsTruncate"),
    ("cellFs", "cellFsFtruncate"),
    ("cellFs", "cellFsChmod"),
    ("cellFs", "cellFsGetFreeSize"),
    ("cellFs", "cellFsGetDirectoryEntries"),

    # ---- cellPad ----
    ("cellPad", "cellPadInit"),
    ("cellPad", "cellPadEnd"),
    ("cellPad", "cellPadClearBuf"),
    ("cellPad", "cellPadGetData"),
    ("cellPad", "cellPadGetDataExtra"),
    ("cellPad", "cellPadGetInfo"),
    ("cellPad", "cellPadGetInfo2"),
    ("cellPad", "cellPadGetCapabilityInfo"),
    ("cellPad", "cellPadSetPortSetting"),
    ("cellPad", "cellPadSetSensorMode"),
    ("cellPad", "cellPadSetActDirect"),
    ("cellPad", "cellPadInfoPressMode"),
    ("cellPad", "cellPadInfoSensorMode"),
    ("cellPad", "cellPadSetPressMode"),

    # ---- cellKb ----
    ("cellKb", "cellKbInit"),
    ("cellKb", "cellKbEnd"),
    ("cellKb", "cellKbClearBuf"),
    ("cellKb", "cellKbRead"),
    ("cellKb", "cellKbGetInfo"),
    ("cellKb", "cellKbSetCodeType"),

    # ---- cellMouse ----
    ("cellMouse", "cellMouseInit"),
    ("cellMouse", "cellMouseEnd"),
    ("cellMouse", "cellMouseClearBuf"),
    ("cellMouse", "cellMouseGetData"),
    ("cellMouse", "cellMouseGetInfo"),

    # ---- cellAudio ----
    ("cellAudio", "cellAudioInit"),
    ("cellAudio", "cellAudioQuit"),
    ("cellAudio", "cellAudioPortOpen"),
    ("cellAudio", "cellAudioPortClose"),
    ("cellAudio", "cellAudioPortStart"),
    ("cellAudio", "cellAudioPortStop"),
    ("cellAudio", "cellAudioGetPortConfig"),
    ("cellAudio", "cellAudioGetPortTimestamp"),
    ("cellAudio", "cellAudioGetPortBlockTag"),
    ("cellAudio", "cellAudioSetNotifyEventQueue"),
    ("cellAudio", "cellAudioRemoveNotifyEventQueue"),
    ("cellAudio", "cellAudioSetPortLevel"),
    ("cellAudio", "cellAudioCreateNotifyEventQueue"),

    # ---- cellSpurs ----
    ("cellSpurs", "cellSpursInitialize"),
    ("cellSpurs", "cellSpursFinalize"),
    ("cellSpurs", "cellSpursAttachLv2EventQueue"),
    ("cellSpurs", "cellSpursDetachLv2EventQueue"),
    ("cellSpurs", "cellSpursGetNumSpuThread"),
    ("cellSpurs", "cellSpursGetSpuThreadGroupId"),
    ("cellSpurs", "cellSpursGetSpuThreadId"),
    ("cellSpurs", "cellSpursSetMaxContention"),
    ("cellSpurs", "cellSpursSetPriorities"),

    # ---- sysPrxForUser ----
    ("sysPrxForUser", "sys_ppu_thread_create"),
    ("sysPrxForUser", "sys_ppu_thread_exit"),
    ("sysPrxForUser", "sys_ppu_thread_join"),
    ("sysPrxForUser", "sys_ppu_thread_yield"),
    ("sysPrxForUser", "sys_ppu_thread_get_id"),
    ("sysPrxForUser", "sys_ppu_thread_get_priority"),
    ("sysPrxForUser", "sys_ppu_thread_set_priority"),
    ("sysPrxForUser", "sys_ppu_thread_get_stack_information"),
    ("sysPrxForUser", "sys_process_exit"),
    ("sysPrxForUser", "sys_process_getpid"),
    ("sysPrxForUser", "sys_process_get_sdk_version"),
    ("sysPrxForUser", "_sys_printf"),
    ("sysPrxForUser", "_sys_sprintf"),
    ("sysPrxForUser", "_sys_snprintf"),
    ("sysPrxForUser", "_sys_strlen"),
    ("sysPrxForUser", "_sys_strcmp"),
    ("sysPrxForUser", "_sys_strncmp"),
    ("sysPrxForUser", "_sys_strcpy"),
    ("sysPrxForUser", "_sys_strncpy"),
    ("sysPrxForUser", "_sys_strcat"),
    ("sysPrxForUser", "_sys_strncat"),
    ("sysPrxForUser", "_sys_memset"),
    ("sysPrxForUser", "_sys_memcpy"),
    ("sysPrxForUser", "_sys_memcmp"),
    ("sysPrxForUser", "_sys_memmove"),
    ("sysPrxForUser", "_sys_malloc"),
    ("sysPrxForUser", "_sys_free"),
    ("sysPrxForUser", "_sys_memalign"),
    ("sysPrxForUser", "sys_prx_exitspawn_with_level"),
    ("sysPrxForUser", "sys_spu_printf_initialize"),
    ("sysPrxForUser", "sys_spu_printf_finalize"),
    ("sysPrxForUser", "sys_prx_load_module"),
    ("sysPrxForUser", "sys_prx_start_module"),
    ("sysPrxForUser", "sys_prx_stop_module"),
    ("sysPrxForUser", "sys_prx_unload_module"),
    ("sysPrxForUser", "sys_prx_register_library"),
    ("sysPrxForUser", "sys_prx_unregister_library"),
    ("sysPrxForUser", "sys_lwmutex_create"),
    ("sysPrxForUser", "sys_lwmutex_destroy"),
    ("sysPrxForUser", "sys_lwmutex_lock"),
    ("sysPrxForUser", "sys_lwmutex_trylock"),
    ("sysPrxForUser", "sys_lwmutex_unlock"),
    ("sysPrxForUser", "sys_lwcond_create"),
    ("sysPrxForUser", "sys_lwcond_destroy"),
    ("sysPrxForUser", "sys_lwcond_wait"),
    ("sysPrxForUser", "sys_lwcond_signal"),
    ("sysPrxForUser", "sys_lwcond_signal_all"),
    ("sysPrxForUser", "sys_mutex_create"),
    ("sysPrxForUser", "sys_mutex_destroy"),
    ("sysPrxForUser", "sys_mutex_lock"),
    ("sysPrxForUser", "sys_mutex_trylock"),
    ("sysPrxForUser", "sys_mutex_unlock"),
    ("sysPrxForUser", "sys_cond_create"),
    ("sysPrxForUser", "sys_cond_destroy"),
    ("sysPrxForUser", "sys_cond_wait"),
    ("sysPrxForUser", "sys_cond_signal"),
    ("sysPrxForUser", "sys_cond_signal_all"),
    ("sysPrxForUser", "sys_semaphore_create"),
    ("sysPrxForUser", "sys_semaphore_destroy"),
    ("sysPrxForUser", "sys_semaphore_wait"),
    ("sysPrxForUser", "sys_semaphore_trywait"),
    ("sysPrxForUser", "sys_semaphore_post"),
    ("sysPrxForUser", "sys_event_flag_create"),
    ("sysPrxForUser", "sys_event_flag_destroy"),
    ("sysPrxForUser", "sys_event_flag_wait"),
    ("sysPrxForUser", "sys_event_flag_set"),
    ("sysPrxForUser", "sys_event_flag_clear"),
    ("sysPrxForUser", "sys_event_queue_create"),
    ("sysPrxForUser", "sys_event_queue_destroy"),
    ("sysPrxForUser", "sys_event_queue_receive"),
    ("sysPrxForUser", "sys_event_port_create"),
    ("sysPrxForUser", "sys_event_port_destroy"),
    ("sysPrxForUser", "sys_event_port_connect_local"),
    ("sysPrxForUser", "sys_event_port_send"),
    ("sysPrxForUser", "sys_timer_create"),
    ("sysPrxForUser", "sys_timer_destroy"),
    ("sysPrxForUser", "sys_timer_start"),
    ("sysPrxForUser", "sys_timer_stop"),
    ("sysPrxForUser", "sys_timer_usleep"),
    ("sysPrxForUser", "sys_timer_sleep"),
    ("sysPrxForUser", "sys_time_get_system_time"),
    ("sysPrxForUser", "sys_time_get_current_time"),
    ("sysPrxForUser", "sys_time_get_timebase_frequency"),
    ("sysPrxForUser", "sys_mmapper_allocate_address"),
    ("sysPrxForUser", "sys_mmapper_free_address"),
    ("sysPrxForUser", "sys_memory_allocate"),
    ("sysPrxForUser", "sys_memory_free"),
    ("sysPrxForUser", "sys_memory_get_user_memory_size"),
    ("sysPrxForUser", "sys_tty_write"),
    ("sysPrxForUser", "sys_tty_read"),
    ("sysPrxForUser", "sys_dbg_register_ppu_exception_handler"),

    # ---- cellResc ----
    ("cellResc", "cellRescInit"),
    ("cellResc", "cellRescExit"),
    ("cellResc", "cellRescSetDisplayMode"),
    ("cellResc", "cellRescGetNumColorBuffers"),
    ("cellResc", "cellRescSetBufferAddress"),
    ("cellResc", "cellRescGetBufferSize"),
    ("cellResc", "cellRescSetFlipHandler"),
    ("cellResc", "cellRescSetVBlankHandler"),
    ("cellResc", "cellRescGcmSurface2RescSrc"),
    ("cellResc", "cellRescSetSrc"),
    ("cellResc", "cellRescSetConvertAndFlip"),

    # ---- cellSysmodule ----
    ("cellSysmodule", "cellSysmoduleLoadModule"),
    ("cellSysmodule", "cellSysmoduleUnloadModule"),
    ("cellSysmodule", "cellSysmoduleIsLoaded"),
    ("cellSysmodule", "cellSysmoduleInitialize"),
    ("cellSysmodule", "cellSysmoduleFinalize"),
]

# ---------------------------------------------------------------------------
# NIDDatabase class
# ---------------------------------------------------------------------------

class NIDDatabase:
    """In-memory NID database supporting lookup by NID or name."""

    def __init__(self):
        # nid -> (module, name)
        self._by_nid: dict[int, tuple[str, str]] = {}
        # name -> nid
        self._by_name: dict[str, int] = {}
        # module -> list of (nid, name)
        self._by_module: dict[str, list[tuple[int, str]]] = {}

    # ---- population ----

    def add(self, module: str, name: str, nid: int | None = None) -> int:
        """Add an entry.  If *nid* is None it will be computed."""
        if nid is None:
            nid = compute_nid(name)
        self._by_nid[nid] = (module, name)
        self._by_name[name] = nid
        self._by_module.setdefault(module, []).append((nid, name))
        return nid

    def load_builtins(self) -> None:
        """Populate with the built-in function list."""
        for module, name in _BUILTIN_FUNCTIONS:
            self.add(module, name)

    def load_json(self, path: str) -> int:
        """Load entries from a JSON file. Returns count loaded.

        Expected format: { "module_name": { "function_name": "0xNID", ... }, ... }
        or a flat list: [ {"module": "...", "name": "...", "nid": "0x..."}, ... ]
        """
        with open(path) as f:
            data = json.load(f)

        count = 0
        if isinstance(data, dict):
            for module, funcs in data.items():
                if isinstance(funcs, dict):
                    for name, nid_str in funcs.items():
                        nid = int(str(nid_str), 0)
                        self.add(module, name, nid)
                        count += 1
        elif isinstance(data, list):
            for entry in data:
                module = entry.get("module", "unknown")
                name = entry["name"]
                nid = int(str(entry.get("nid", "0")), 0) if "nid" in entry else None
                self.add(module, name, nid)
                count += 1
        return count

    def save_json(self, path: str) -> None:
        """Save the database as JSON (grouped by module)."""
        out: dict[str, dict[str, str]] = {}
        for module, entries in sorted(self._by_module.items()):
            mod_dict: dict[str, str] = {}
            for nid, name in sorted(entries, key=lambda x: x[1]):
                mod_dict[name] = f"0x{nid:08X}"
            out[module] = mod_dict
        with open(path, "w") as f:
            json.dump(out, f, indent=2)

    # ---- lookup ----

    def lookup_nid(self, nid: int) -> tuple[str, str] | None:
        """Return (module, name) for a NID, or None."""
        return self._by_nid.get(nid)

    def lookup_name(self, name: str) -> int | None:
        """Return the NID for a function name, or None."""
        return self._by_name.get(name)

    def resolve_nids(self, nids: list[int]) -> dict[int, tuple[str, str] | None]:
        """Batch-resolve a list of NIDs."""
        return {nid: self.lookup_nid(nid) for nid in nids}

    def all_entries(self) -> list[tuple[int, str, str]]:
        """Return all (nid, module, name) tuples."""
        return [(nid, mod, name) for nid, (mod, name) in sorted(self._by_nid.items())]

    @property
    def size(self) -> int:
        return len(self._by_nid)

    # ---- module queries ----

    def modules(self) -> list[str]:
        return sorted(self._by_module.keys())

    def module_entries(self, module: str) -> list[tuple[int, str]]:
        return self._by_module.get(module, [])

# ---------------------------------------------------------------------------
# Singleton convenience
# ---------------------------------------------------------------------------

_default_db: NIDDatabase | None = None


def get_default_db() -> NIDDatabase:
    """Return a lazily-initialised default database with built-in entries."""
    global _default_db
    if _default_db is None:
        _default_db = NIDDatabase()
        _default_db.load_builtins()
    return _default_db

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="PS3 NID resolution database")
    parser.add_argument("--lookup", metavar="NID",
                        help="Look up a NID (hex, e.g. 0xAB8B4DA4)")
    parser.add_argument("--name", metavar="NAME",
                        help="Look up a function name -> NID")
    parser.add_argument("--compute", metavar="NAME",
                        help="Compute NID for an arbitrary name (not in DB)")
    parser.add_argument("--batch", metavar="FILE",
                        help="Resolve NIDs from a file (one hex NID per line)")
    parser.add_argument("--dump", action="store_true",
                        help="Dump entire database")
    parser.add_argument("--dump-module", metavar="MODULE",
                        help="Dump entries for a specific module")
    parser.add_argument("--load", metavar="FILE",
                        help="Load additional entries from JSON file")
    parser.add_argument("--save", metavar="FILE",
                        help="Save database to JSON file")
    parser.add_argument("--json", action="store_true",
                        help="Output in JSON format")
    args = parser.parse_args()

    db = NIDDatabase()
    db.load_builtins()

    if args.load:
        count = db.load_json(args.load)
        print(f"Loaded {count} entries from {args.load}", file=sys.stderr)

    if args.save:
        db.save_json(args.save)
        print(f"Saved {db.size} entries to {args.save}")
        return

    if args.compute:
        nid = compute_nid(args.compute)
        if args.json:
            print(json.dumps({"name": args.compute, "nid": f"0x{nid:08X}"}))
        else:
            print(f"{args.compute} -> 0x{nid:08X}")
        return

    if args.lookup:
        nid = int(args.lookup, 0)
        result = db.lookup_nid(nid)
        if result:
            module, name = result
            if args.json:
                print(json.dumps({"nid": f"0x{nid:08X}", "module": module, "name": name}))
            else:
                print(f"0x{nid:08X} -> {module}::{name}")
        else:
            if args.json:
                print(json.dumps({"nid": f"0x{nid:08X}", "module": None, "name": None}))
            else:
                print(f"0x{nid:08X} -> (unknown)")
        return

    if args.name:
        nid = db.lookup_name(args.name)
        if nid is not None:
            result = db.lookup_nid(nid)
            module = result[0] if result else "?"
            if args.json:
                print(json.dumps({"name": args.name, "module": module, "nid": f"0x{nid:08X}"}))
            else:
                print(f"{args.name} ({module}) -> 0x{nid:08X}")
        else:
            # Compute it anyway
            computed = compute_nid(args.name)
            if args.json:
                print(json.dumps({"name": args.name, "module": None,
                                  "nid": f"0x{computed:08X}", "note": "computed, not in DB"}))
            else:
                print(f"{args.name} -> 0x{computed:08X}  (computed, not in built-in DB)")
        return

    if args.batch:
        with open(args.batch) as f:
            nid_strs = [line.strip() for line in f if line.strip()]
        nids = [int(s, 0) for s in nid_strs]
        results = db.resolve_nids(nids)
        if args.json:
            out = {}
            for nid, res in results.items():
                key = f"0x{nid:08X}"
                if res:
                    out[key] = {"module": res[0], "name": res[1]}
                else:
                    out[key] = None
            print(json.dumps(out, indent=2))
        else:
            for nid, res in results.items():
                if res:
                    print(f"0x{nid:08X} -> {res[0]}::{res[1]}")
                else:
                    print(f"0x{nid:08X} -> (unknown)")
        return

    if args.dump_module:
        entries = db.module_entries(args.dump_module)
        if not entries:
            print(f"No entries for module '{args.dump_module}'", file=sys.stderr)
            sys.exit(1)
        if args.json:
            out = {name: f"0x{nid:08X}" for nid, name in sorted(entries, key=lambda x: x[1])}
            print(json.dumps({args.dump_module: out}, indent=2))
        else:
            print(f"Module: {args.dump_module}  ({len(entries)} entries)")
            for nid, name in sorted(entries, key=lambda x: x[1]):
                print(f"  0x{nid:08X}  {name}")
        return

    if args.dump:
        all_entries = db.all_entries()
        if args.json:
            out: dict[str, dict[str, str]] = {}
            for nid, module, name in all_entries:
                out.setdefault(module, {})[name] = f"0x{nid:08X}"
            print(json.dumps(out, indent=2))
        else:
            print(f"NID Database: {len(all_entries)} entries")
            current_module = None
            for nid, module, name in sorted(all_entries, key=lambda x: (x[1], x[2])):
                if module != current_module:
                    print(f"\n[{module}]")
                    current_module = module
                print(f"  0x{nid:08X}  {name}")
        return

    # Default: print summary
    print(f"NID Database: {db.size} entries across {len(db.modules())} modules")
    for mod in db.modules():
        entries = db.module_entries(mod)
        print(f"  {mod}: {len(entries)} functions")
    print("\nUse --help for usage information.")


if __name__ == "__main__":
    main()
