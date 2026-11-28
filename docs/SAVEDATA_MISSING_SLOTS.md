# Missing automatic save slots

`cellSaveDataAutoLoad2` now preserves `CELL_SAVEDATA_ERROR_NODATA` by default.
Reporting success after an absent slot leaves the caller's previous file buffer
unchanged. Eternal Sonata interpreted that as another valid preview, showing
copies of slot 1 in empty slots.

The previous unconditional success override was a compatibility workaround for
flOw's MODE_AUTO_LOAD state machine. Ports relying on that behavior must set
`PS3_SAVEDATA_AUTOLOAD_NODATA_OK=1` in their launcher before calling AutoLoad2.
For example, Windows batch: `set PS3_SAVEDATA_AUTOLOAD_NODATA_OK=1`.
Only the exact value `1` enables the override. Other errors and callback behavior
are unchanged. This is a process-wide compatibility option, not a save format change.

Run `python tools/test_savedata_autoload_missing.py` for stat/file callbacks,
missing-after-present preview handling, explicit compatibility, error propagation,
and a real file save/load round trip in an automatically removed temporary directory.
The compatibility path is covered synthetically; an actual flOw game boot has not
been validated for this proposal.
