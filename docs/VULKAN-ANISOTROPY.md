# Vulkan anisotropic filtering

The renderer now carries fragment texture CONTROL0 bits 4..6 through the draw
engine and both Vulkan sampler paths. The encodings select 1,2,4,6,8,10,12,16x.
Vertex samplers retain 1x. Device creation enables samplerAnisotropy only when
reported, and effective levels are limited to the device maximum and 16x.
The sampler cache includes effective anisotropy values; the legacy key includes
the guest encoding. Other sampler state remains unchanged.

`PS3RECOMP_ANISOTROPY=auto` (or 0/unset) honors the guest. Values 1,2,4,8,16 are
startup overrides; 1 disables anisotropy. Overrides above 1 apply only to
fragment samplers with both minification and magnification set to linear,
preserving nearest-filtered UI. Invalid settings fail initialization visibly.
Device capabilities and the requested override are logged. Player-facing
selection and persistence of this additional option remain pending.

References: [RPCS3 texture decode](https://github.com/RPCS3/rpcs3/blob/4d5def5880217135554ed434cd5055cceebe9cc2/rpcs3/Emu/RSX/RSXTexture.cpp)
and [Vulkan sampler requirements](https://registry.khronos.org/vulkan/specs/latest/man/html/VkSamplerCreateInfo.html).
The register facts were verified against the pinned GPL-2.0 RPCS3 reference;
no RPCS3 implementation was copied for this change.

Claude job-uq5V51 authored the selection helper and CPU suite (actual Opus5.5).
Coordinator review corrected invalid guest codes being re-enabled by an override,
added its regression, and integrated the renderer paths and tests. The helper
passes 693 checks on Windows and Linux, the actual backend cache passes 12 CPU
checks, and the engine passes 142 checks, including all eight guest encodings.

`test_vulkan_anisotropy_backend --cpu` never initializes graphics. Its explicit
`--gpu` mode renders two synthetic stripe samples and saves actual readbacks.
The GPU fixture is compiled but awaits the exclusive GPU controller. Matched
gameplay image quality and performance evidence remain required for acceptance.
