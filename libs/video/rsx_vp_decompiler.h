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

/* Point size, Metal only.
 *
 * Metal leaves a point's size undefined unless the vertex function writes
 * [[point_size]], and the decompiled VSOutput has no such member. The
 * pieces below add one behind RSX_POINT_SIZE_BUILTIN. Only the HLSL-to-MSL
 * translator defines it (a glslang preamble in rsx_shader_msl.cpp), so the
 * D3D12 and Vulkan paths compile the text exactly as before: no member and
 * no extra constant row.
 *
 * glslang's HLSL front end does not map the PSIZE semantic to the PointSize
 * built-in by itself (checked on glslang 16.6); [[vk::builtin("PointSize")]]
 * does, and spirv-cross then needs MSL_ENABLE_POINT_SIZE_BUILTIN to spell it
 * [[point_size]].
 *
 * vp_pointsize is one more float4 after vp_posoffset in VPConst:
 *   .x  NV4097_SET_POINT_SIZE (0x1EE0), in pixels
 *   .y  1 when the vertex program writes the size (bit 5 of
 *       NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, 0x1FF4: the point-size
 *       output, o[6].x), else 0
 *   .z  largest size drawn, in pixels (RSX_VP_POINT_SIZE_MAX)
 *   .w  unused
 * The draw engine fills the row; a backend binding the translated MSL
 * supplies it as part of VPConst.
 *
 * Where the numbers come from:
 *   - the registers: docs/rsxfifo.txt in this tree lists SET_POINT_SIZE at
 *     0x1EE0 and SET_VERTEX_ATTRIB_OUTPUT_MASK at 0x1FF4; envytools'
 *     rnndb/graph/nv30-40_3d.xml (MIT, per rnndb/copyright.xml) types 0x1EE0
 *     POINT_SIZE as a float and names 0x1FF4 VP_RESULT_EN;
 *   - the output mask bit and the o[6] register: Mesa's nouveau nv30 driver
 *     (Mesa's MIT licence; these two files carry no notice of their own)
 *     numbers the vertex program's outputs POS 0, COL0 1, COL1 2, BFC0 3,
 *     BFC1 4, FOGC 5, PSZ 6, TC0 7.. (nv40_vertprog.h, NV40_VP_INST_DEST_*),
 *     sets bit (dest - 1) of the mask for each of COL0..PSZ a program
 *     writes, so PSZ is bit 5 (nvfx_vertprog.c), and uploads that mask to
 *     VP_RESULT_EN (nv30_vertprog.c). Only those facts are used, not the
 *     code;
 *   - the 64 pixel ceiling: Mesa's nouveau nv30 driver (nv30_screen.c)
 *     reports max_point_size 64.0 for NV3x/NV4x-class hardware, and a
 *     minimum of 1, which the 1..64 clamp follows. It is that driver's
 *     advertised limit, not a figure measured on an RSX.
 */
#define RSX_VP_POINT_SIZE_ROW_FLOATS 4u
/* NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK bit 5: the vertex program writes the
 * point size (o[6].x). */
#define RSX_VP_OUTPUT_MASK_POINT_SIZE 0x20u
/* Largest point drawn, in pixels: the limit Mesa's nv30 driver advertises. */
#define RSX_VP_POINT_SIZE_MAX 64.0f
#define RSX_VP_HLSL_PSIZE_MEMBER \
    "#ifdef RSX_POINT_SIZE_BUILTIN\n" \
    "    [[vk::builtin(\"PointSize\")]] float psz:PSIZE;\n" \
    "#endif\n"
#define RSX_VP_HLSL_PSIZE_ROW \
    "#ifdef RSX_POINT_SIZE_BUILTIN\n" \
    "    float4 vp_pointsize;\n" \
    "#endif\n"
#define RSX_VP_HLSL_PSIZE_FUNC \
    "#ifdef RSX_POINT_SIZE_BUILTIN\n" \
    "float vp_point_size(float from_vp) {\n" \
    "    float s = (vp_pointsize.y != 0.0) ? from_vp : vp_pointsize.x;\n" \
    "    s = (s == s) ? s : 1.0;\n" \
    "    return min(max(s, 1.0), max(vp_pointsize.z, 1.0));\n" \
    "}\n" \
    "#endif\n"
/* The assignment; `from_vp` is a string literal for the program's own size. */
#define RSX_VP_HLSL_PSIZE_STORE(from_vp) \
    "#ifdef RSX_POINT_SIZE_BUILTIN\n" \
    "    Out.psz = vp_point_size(" from_vp ");\n" \
    "#endif\n"

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

/* The VP slot the next program decompiled starts at (NV4097 transform
 * program start). Branch targets are absolute slots; this makes them relative
 * to the ucode pointer handed in. Defaults to 0. */
void rsx_vp_set_branch_base(u32 start_slot);

/* SET_TRANSFORM_BRANCH_BITS for the next program decompiled. BRB and CLB
 * branch on one of these bits; the decompiler bakes the outcome in, so a
 * program that rsx_vp_uses_branch_bits() must be keyed on them too. */
void rsx_vp_set_branch_bits(u32 bits);
int rsx_vp_uses_branch_bits(const u8* ucode, u32 max_bytes);

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

/* User clip planes. RSX vertex programs write the six clip distances into
 * o5.yzw and o6.yzw (o5.x is fog, o6.x the point size); which planes count,
 * and which side of each is kept, is NV4097_SET_USER_CLIP_PLANE_CONTROL
 * (0x1478) state, not program state. `ops` holds two bits per plane, plane N
 * at bits 2N..2N+1: 0 disabled, 1 ENABLE_LT (keeps d <= 0), 2 ENABLE_GE
 * (keeps d >= 0) -- RPCS3's factors -1/+1. Adds SV_ClipDistance outputs for
 * them to HLSL from rsx_vp_decompile*; a disabled plane in the same vector
 * gets 0.5 and never clips. Returns 0 (nothing to do when ops is 0), or -1
 * when the text is not in the decompiler's form or does not fit in cap. */
int rsx_vp_apply_user_clip(char* hlsl, size_t cap, u32 ops);

/* Mnemonics for the vector / scalar opcode fields ("?" if unknown). */
const char* rsx_vp_vec_name(u32 op);
const char* rsx_vp_sca_name(u32 op);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
