/*
 * ps3recomp - HLSL -> Metal Shading Language translation
 *
 * The RSX decompilers (rsx_vp_decompiler.c, rsx_fp_decompiler.c) emit HLSL,
 * because the D3D12 backend compiles that directly. Metal cannot. Rather than
 * grow a second pair of decompilers that emit MSL and drift from the first,
 * the HLSL is lowered with tools that already exist for exactly this job:
 * glslang parses the HLSL and produces SPIR-V, spirv-cross turns the SPIR-V
 * into MSL. The Metal backend then compiles that MSL at runtime with
 * -newLibraryWithSource:, which needs no Xcode.
 *
 * Both libraries are optional at build time. When CMake does not find them
 * the module compiles to a stub that reports itself unavailable, and the
 * Metal backend stays on its fixed-function path. That keeps a plain
 * `cmake && cmake --build` working on a machine without the packages while
 * CI, which has them, checks the real thing.
 *
 * Binding contract, which the Metal backend relies on:
 *   register(bN)  ->  [[buffer(N)]]      (VPConst b0, PSConstants b1)
 *   register(tN)  ->  [[texture(N)]]
 *   register(sN)  ->  [[sampler(N)]]
 *   ATTRn         ->  [[attribute(n)]]   (vertex inputs, in declaration order)
 *   COLOR0/COLOR1/FOG/TEXCOORD0..7 -> [[user(locn0..10)]], numbered in the
 *   declaration order both decompilers share, so the stages link.
 * The MSL entry point is always `main0`.
 */
#ifndef PS3RECOMP_RSX_SHADER_MSL_H
#define PS3RECOMP_RSX_SHADER_MSL_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SHADER_STAGE_VERTEX   0
#define RSX_SHADER_STAGE_FRAGMENT 1

/* 1 when the translator was built with glslang and spirv-cross, 0 when the
 * stub is in. A backend asks this once and picks its shader path. */
int rsx_hlsl_to_msl_available(void);

/* Translate one HLSL shader whose entry point is `main` into MSL.
 *   hlsl     : NUL-terminated HLSL source, as the decompilers emit it.
 *   stage    : RSX_SHADER_STAGE_VERTEX or RSX_SHADER_STAGE_FRAGMENT.
 *   out      : receives the NUL-terminated MSL.
 *   log      : receives a diagnostic on failure (may be NULL).
 * Returns 0 on success, -1 on failure (bad input, a front-end error, an
 * output buffer too small, or the stub). */
int rsx_hlsl_to_msl(const char* hlsl, int stage,
                    char* out, u32 out_size,
                    char* log, u32 log_size);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_SHADER_MSL_H */
