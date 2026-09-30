/*
 * ps3recomp - HLSL -> SPIR-V for the Vulkan backend
 *
 * The RSX decompilers emit HLSL. The Metal backend lowers it HLSL -> SPIR-V ->
 * MSL (rsx_shader_msl.cpp); Vulkan consumes SPIR-V directly, so this stops
 * one step earlier and needs glslang only, not spirv-cross. Without glslang
 * the module compiles to a stub that reports itself unavailable, and the
 * Vulkan backend stays on its fixed-function path.
 *
 * Binding contract, which the Vulkan backend relies on (all in set 0). Vulkan
 * has one binding namespace per set where MSL has three, so the register
 * classes are moved apart:
 *   register(bN)  ->  binding N                          (VPConst 0, PSConstants 1)
 *   register(tN)  ->  binding RSX_SPIRV_TEXTURE_BINDING + N
 *   register(sN)  ->  binding RSX_SPIRV_SAMPLER_BINDING + N
 * The fragment decompiler's `Texture2D rsx_tex[16] : register(t0)` is one
 * binding holding an array of 16, and `SamplerState rsx_samp[16]` likewise.
 * Vertex-texture units (t16..t19) land on TEXTURE_BINDING + 16..19.
 *
 * Locations are numbered in declaration order (TProgram::mapIO). The vertex
 * decompiler always declares ATTR0..ATTR15 in order, so vertex input
 * location n is RSX attribute n; both decompilers declare the varyings in the
 * same order, so the stages link.
 *
 * The vertex stage's position is flipped in Y: the decompiled HLSL writes
 * D3D clip space (+y up), and Vulkan's framebuffer has +y down. Depth needs
 * nothing -- the viewport epilogue already maps GL clip z to [0, 1].
 */

#ifndef PS3RECOMP_RSX_SHADER_SPIRV_H
#define PS3RECOMP_RSX_SHADER_SPIRV_H

#include "ps3emu/ps3types.h"
#include "rsx_shader_msl.h"   /* RSX_SHADER_STAGE_VERTEX / _FRAGMENT */

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SPIRV_VPCONST_BINDING  0u
#define RSX_SPIRV_PSCONST_BINDING  1u
#define RSX_SPIRV_TEXTURE_BINDING  16u
#define RSX_SPIRV_SAMPLER_BINDING  48u

/* 1 when built with glslang, 0 when the stub is in. */
int rsx_hlsl_to_spirv_available(void);

/* Translate one HLSL shader whose entry point is `main` into SPIR-V 1.0 for a
 * Vulkan 1.0 client.
 *   hlsl      : NUL-terminated HLSL, as the decompilers emit it.
 *   stage     : RSX_SHADER_STAGE_VERTEX or RSX_SHADER_STAGE_FRAGMENT.
 *   words     : receives the SPIR-V, max_words 32-bit words at most.
 *   out_words : receives the word count.
 *   log       : receives a diagnostic on failure (may be NULL).
 * Returns 0 on success, -1 on failure (bad input, a front-end error, an
 * output buffer too small, or the stub). */
int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                      u32* words, u32 max_words, u32* out_words,
                      char* log, u32 log_size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SHADER_SPIRV_H */
