/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX vertex shader (NVIDIA NV40 "nvfx" vertex program, a VLIW
 * that co-issues one vector ALU op and one scalar ALU op per instruction) into
 * an HLSL vertex shader.
 *
 * Encoding reference: RPCS3 RSXVertexProgram.h (D0..D3 bitfields) + Mesa
 * nouveau nvfx_shader.h opcode tables. Words are stored little-endian (RPCS3
 * native order), 4 words / 16 bytes per instruction, no inline constants
 * (VP constants live in a separate bank), program ends at the D3.end bit.
 *
 * Self-contained: no D3D12 dependency, build/test standalone.
 */
#ifndef PS3RECOMP_RSX_VP_DECOMPILER_H
#define PS3RECOMP_RSX_VP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of instructions in the program at `ucode` (each 16 bytes), up to and
 * including the one with the D3.end bit, bounded by max_bytes. 0 if none. */
u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes);

typedef struct rsx_vp_input_analysis {
    u32 input_mask;
    int exact;
} rsx_vp_input_analysis;

/* Report the guest input registers statically read by the program.  `exact`
 * is zero and input_mask is 0xFFFF when an unsupported opcode/control-flow
 * construct makes the result unsafe to narrow.  Returns the instruction
 * count, or -1 for invalid/non-terminated input. */
int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis);

/* Decompile an NV40 vertex program into an HLSL vertex shader.
 *   ucode    : VP bytecode (little-endian words, as in RPCS3's shader cache).
 *   max_bytes: safety bound.
 *   out      : caller buffer for generated HLSL (NUL-terminated).
 *   out_size : size of out in bytes.
 * Returns instruction count (>=0), or -1 on error (null args / overflow).
 * Emits `VSOutput main(VSInput input)`: 16 float4 inputs (ATTR0..15), a
 * `cbuffer VPConst : register(b0)` with 512 vec4 constants + vp_posscale/
 * vp_posoffset (the RSX viewport transform mapped to D3D clip space; the
 * caller computes them per draw), SV_Position + COLOR0/1 + FOG + TEXCOORD0..7
 * varyings routed per the NV40 output register map (o0/o1/o2/o5/o7..o14).
 * Not modeled: condition-code tests and flow control (BRA/CAL/...). TXL is
 * stubbed to zero unless rsx_vp_decompile_ex receives a bound-unit mask. */
int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size);

/* Bit N in vtex_mask declares NV40 2D vertex-texture unit N at t(16+N),
 * sampler sN, and turns TXL for that unit into SampleLevel(..., LOD 0).
 * Unmasked units retain the defined-zero fallback. */
int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size);

/* Compact-input variant.  The generated VSInput declares only the original
 * ATTRn semantics selected by input_mask.  The mask is checked against a
 * fresh static analysis; any uncertainty or missing referenced bit falls
 * back to all 16 inputs. */
int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size);

/* Mnemonics for the vector / scalar opcode fields ("?" if unknown). */
const char* rsx_vp_vec_name(u32 op);
const char* rsx_vp_sca_name(u32 op);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
