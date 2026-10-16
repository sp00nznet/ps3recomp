# Optional CACAO component

The unchanged pinned implementation is wrapped by `rsx_cacao_impl.cpp` and
the dynamically resolved `rsx_cacao_dispatch` table. It requires no direct
Vulkan-loader link. The existing backend remains responsible for loading
Vulkan, device features, image formats and queue compatibility.

Generate the validated shader pack with `tools/build_cacao_shaders.py`.
Configure `PS3RECOMP_RSX_VULKAN=ON`, `PS3RECOMP_RSX_CACAO=ON`, and
`PS3RECOMP_CACAO_SHADER_DIR` pointing to that generated directory. CACAO is
off by default. The build option supplies the component; it does not enable
AO on game draws. No game AO setting or capability fallback exists yet.

Before constructing a CACAO context, bind the dispatch with the existing
instance, device and instance resolver. A failed bind preserves the previous
table. Do not use that previous table with the failed new device. Binding and
reset are externally serialized and require that no old context or recorded
command still uses the table. Destroy screen resources/context and finish
commands before resetting it or destroying the device.

The dynamic test variant is `tests/test_cacao_vulkan.cpp` compiled with
`RSX_CACAO_DYNAMIC_DISPATCH`, the wrapper, dispatch source and upstream
`ffx_cacao.cpp`. Its test harness links Vulkan to create a device, but the
CACAO component itself has no direct Vulkan symbols. The two variants produce
identical synthetic outputs on the tested CPU Vulkan implementation.

`rsx_ao_position_program.h` identifies four position writes using a stable
input or temporary value and contiguous matrix rows. It supports DP4 and DPH
after straight-line skinning calculations, tracks component writes and rejects
control flow or ambiguous position output. No constant base is hardcoded.
`rsx_ao_projection.h` then derives a centered perspective from those live
constant rows and actual viewport fixup. It rejects non-rigid transforms,
reversed depth, invalid matrices and mismatched logical aspect. Expanded MSAA
storage dimensions are not logical camera dimensions.

With the CACAO build option, the actual engine records accepted/unknown draws
and projection conflicts against each depth image. A far-depth clear starts
the lifetime. Every depth-writing draw must use a supported matching projection
in that frame; an unknown draw, changed dimensions/projection, stale frame or
overlapping notified guest write blocks AO use until another clear. Pass dumps
include `ao-camera` metadata, and shader dumps include private `.vp.bin` files
for local verification. No guest bytecode belongs in a distributed patch.
The live game has not yet qualified this camera association. Draws outside
the supported subset disable AO for that image rather than guessing.

Controller tests exercise projection extraction and transactional dispatch.
The original managed implementation of the dispatch is Claude Opus 5.5 job
`job-cBjM5k`, reviewed and tested by the controller. Upstream CACAO remains
MIT licensed with its existing notices. No game or firmware bytes are included.

Pending integration: qualify the live same-frame camera association,
resolve the actual sample grid, apply AO to world color before the guest HUD,
preserve guest resources, handle device/resize/failure paths, expose saved
settings, then measure matched gameplay appearance and cost on hardware.
