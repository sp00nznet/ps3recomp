# Raw depth capture

Set `RSX_ENGINE_PASS_DUMP_DEPTH_RAW=1` alongside the existing bounded pass
capture. Each successfully read depth target can produce `passNNN_z.f32le`:
IEEE754 binary32, little endian, no header, row y=0 first. Dimensions and
write status appear in `passes.txt`. The existing PGM remains a stretched
preview, not a substitute for raw depth.

This reuses the existing backend readback. It does not add GPU submissions,
resolve or average samples, modify depth, or enable ambient occlusion.
Multisample layout is the backend's expanded guest sample grid; consumers
must use the associated surface layout and viewport, not assume one sample
per logical pixel. The optional output is limited to 16 MiB per file and
256 MiB per capture. Attempted writes consume the budget even if they fail.
Ordinary runs and pass captures without the exact flag value `1` are unchanged.

`tests/test_rsx_capture_depth.c` checks exact byte representation, signed zero,
nonfinite bit patterns, chunk boundaries and invalid/write-failure cases.
`tests/test_rsx_capture_depth_engine.c` includes the actual engine with a
deterministic backend readback and checks default-off behavior, enabled output,
metadata, cumulative budget and unchanged readback/submission counts. Its
Linux build uses section garbage collection and requires no GPU or guest data.

These are diagnostic CPU tests. Actual gameplay raw-depth reconstruction,
projection selection, HUD separation and AO quality/performance need separate
validation before an AO mode can be accepted.
