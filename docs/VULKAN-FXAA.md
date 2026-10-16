# Optional FXAA postprocess

`PS3RECOMP_ANTIALIASING=fxaa` requests NVIDIA FXAA 3.11 quality preset 29.
`off` is the default. Other values fail before graphics initialization.
The pass filters the final RGBA8 display image after guest rendering and before
both readback and window presentation. Original guest render targets and the
unfiltered display image remain unchanged. Guest MSAA remains independent.

The input is the existing display-referred 8-bit RGB image. The integration
derives luma from all three RGB channels (0.299, 0.587, 0.114), with gather
disabled, and preserves original alpha at every output pixel. It does not
interpret game alpha as luma. These are explicit adaptations of the vendor
shader, made by the generator without changing the retained original header.
The filter can soften text and other UI in the composite; matched game scenes
must establish whether the tradeoff is useful. It does not add temporal history,
motion vectors, reconstructed texture detail or a temporal-AA mode.

A private RGBA8 target, clamp/linear sampler and graphics pipeline are allocated
once at initialization. Required format features and device dimensions are
checked. A requested setup or frame failure is reported rather than silently
disabling the option. `off` allocates no FXAA resources. Shutdown also handles
partially created resources. The small extra target follows the output size;
it does not multiply the selected internal raster scale again.

The original shader is retained in `third_party/fxaa/Fxaa3_11.h` with its full
NVIDIA notice and three-clause redistribution terms. Source: Intel GameTechDev
CMAA2 commit `071c6b0857559f4e36f614362e6d2aab1b61938a`,
[Fxaa3_11.h](https://github.com/GameTechDev/CMAA2/blob/071c6b0857559f4e36f614362e6d2aab1b61938a/Projects/CMAA2/FXAA/Fxaa3_11.h).
The sample includes Intel's optional separate-luma path; this integration does
not enable it. Exact original download hash is in `third_party/fxaa/source.json`.

Regenerate the embedded Vulkan 1.0 modules with:

```text
python tools/embed_fxaa.py --compiler /absolute/path/to/glslangValidator --work-dir /absolute/task/build-directory
```

The generator verifies the pinned header, applies three counted substitutions
for RGB luma, compiles both shaders and emits `libs/video/rsx_fxaa_spv.h`.
Retain the original notice in source and distribution documentation.

`test_vulkan_fxaa_gpu` tests actual filtering on constant and slanted RGB edges,
alpha/source preservation, repeat frames, odd and tiny targets, production
presentation readback and injected initialization failure. Compilation alone
does not establish rendered behavior. Game quality, performance, saved-setting
runtime checks and target hardware qualification remain separate acceptance work.
