# FSR 1 renderer component

AMD's EASU and RCAS headers are pinned unchanged to the revision and hashes in
source.json. LICENSE.txt contains the upstream MIT notice. The local
.gitattributes file preserves downloaded bytes.

The GLSL wrappers in libs/video/shaders use the FP32 implementation. They need
one sampled display-encoded RGBA8_UNORM image and no optional FP16, subgroup or
storage-image feature. EASU takes 64 bytes of push constants; RCAS takes 16.
The source sampler must use linear filtering and clamp-to-edge addressing.
RCAS clamps integer fetches explicitly. EASU scales alpha bilinearly; RCAS
copies center alpha without sharpening it.

rsx_fsr1_make_constants calls the upstream setup functions for complete images.
It rejects downscaling, zero or inexact FP32 dimensions and sharpness outside
0..2 stops. Device image-size and format checks belong to the Vulkan backend.
The upstream-supported A_STATIC and A_RESTRICT overrides keep its CPU header
compatible with strict C warning checks without editing vendor code.

Regenerate rsx_fsr1_spv.h with tools/embed_fsr1.py, supplying glslangValidator,
spirv-val, spirv-dis and a temporary work directory. The script verifies vendor
hashes, compiles for Vulkan 1.0, validates SPIR-V and checks descriptors, push
offsets and capabilities. tests/test_fsr1_constants.c checks pixel-center
mapping, sharpness and rejection behavior.

The Vulkan draw-engine display path now applies EASU and RCAS to the original
guest display surface before enlargement. Set PS3RECOMP_UPSCALER=fsr1 to opt in;
off is the default. See docs/VULKAN-FSR1.md for fallbacks and validation limits.
FSR 1 is spatial reconstruction. It does not provide temporal reconstruction
or replace the separate motion-vector/depth work needed for DLSS.
