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

`rsx_ao_projection.h` conservatively recognizes four DP4 position writes and
derives a centered perspective from the associated constant rows and actual
viewport fixup. It rejects unknown programs, non-rigid transforms, reversed
depth, invalid matrices and mismatched logical aspect. It supports only the
documented initial subset, is not yet connected to draw/depth lifetimes, and
does not establish correct projection selection for an entire game frame.
Expanded MSAA storage dimensions are not logical camera dimensions.

Controller tests exercise projection extraction and transactional dispatch.
The original managed implementation of the dispatch is Claude Opus 5.5 job
`job-cBjM5k`, reviewed and tested by the controller. Upstream CACAO remains
MIT licensed with its existing notices. No game or firmware bytes are included.

Pending integration: associate each depth image with its same-frame camera,
resolve the actual sample grid, apply AO to world color before the guest HUD,
preserve guest resources, handle device/resize/failure paths, expose saved
settings, then measure matched gameplay appearance and cost on hardware.
