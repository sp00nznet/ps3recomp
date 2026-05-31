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

/* Decompile an NV40 vertex program into an HLSL vertex shader.
 *   ucode    : VP bytecode (little-endian words, as in RPCS3's shader cache).
 *   max_bytes: safety bound.
 *   out      : caller buffer for generated HLSL (NUL-terminated).
 *   out_size : size of out in bytes.
 * Returns instruction count (>=0), or -1 on error (null args / overflow).
 * Emits `VSOutput main(VSInput input)` writing SV_Position + color/texcoord
 * varyings. Semantics are best-effort (compiles cleanly); exact output routing
 * and rare ops are approximate pending a pixel-level diff vs RPCS3. */
int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size);

/* Mnemonics for the vector / scalar opcode fields ("?" if unknown). */
const char* rsx_vp_vec_name(u32 op);
const char* rsx_vp_sca_name(u32 op);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
