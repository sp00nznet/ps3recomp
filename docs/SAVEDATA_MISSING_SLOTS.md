# Missing automatic save slots

`cellSaveDataAutoLoad2` preserves `CELL_SAVEDATA_ERROR_NODATA` by default.
Reporting success after an absent slot leaves the caller's previous file buffer
unchanged. Eternal Sonata interpreted that as another valid preview, showing
copies of slot 1 in empty slots.

Ports relying on the previous AutoLoad2 override can set
`PS3_SAVEDATA_AUTOLOAD_NODATA_OK=1` before the call (Windows batch:
`set PS3_SAVEDATA_AUTOLOAD_NODATA_OK=1`). Only the exact value `1` enables it.
Other errors and callback behavior are unchanged. This is a process-wide
AutoLoad2 compatibility option, not a save format change.

## Legacy AutoLoad compatibility

`cellSaveDataAutoLoad` is a separate implementation and keeps its existing
NODATA-to-OK workaround regardless of this environment variable. The public
[flOw port](https://github.com/sp00nznet/flow/blob/3163de5ae40c0515c329cc40f2236c5873d6de44/src/hle_modules.cpp#L2095)
imports that legacy entry (NID `0xc22c79b5`), not AutoLoad2. Its current bridge
does not require a launcher migration for this change. This deliberately leaves
the older API's compatibility behavior unchanged; it does not fix every save API.

Run `python tools/test_savedata_autoload_missing.py` for both production entry
points: missing slots and stat callbacks, successful file reads, missing after
present, unrelated error propagation, and unset/0/1/true/01 option values.
The test uses real file save/load round trips in an automatically removed
temporary directory. It checks API compatibility, not full flOw gameplay;
no actual flOw game boot is claimed.
