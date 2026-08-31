/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX fragment shader (NVIDIA NV40 "nvfx" fragment program
 * encoding) into an HLSL pixel shader source string suitable for D3DCompile.
 *
 * See docs/RSX_FRAGMENT_PROGRAM.md for the instruction encoding reference
 * (sourced from Mesa/nouveau nvfx_shader.h). This module is self-contained:
 * it has no D3D12 dependency and can be built/tested standalone.
 */

#ifndef PS3RECOMP_RSX_FP_DECOMPILER_H
#define PS3RECOMP_RSX_FP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_FP_BUFFERED_CONSTANTS_API 1
#define RSX_FP_MAX_INLINE_CONSTANTS 2048u

/* Runtime payload for a structurally decompiled fragment program. Values are
 * retained as host-order IEEE-754 bit patterns so signed zero, infinities,
 * NaNs, and NaN payloads survive the guest-word conversion exactly. One slot
 * is consumed per instruction that references CONST; multiple CONST operands
 * in that instruction share the slot, matching the RSX encoding. */
typedef struct rsx_fp_constant_block {
    u32 count;
    u32 values[RSX_FP_MAX_INLINE_CONSTANTS][4];
} rsx_fp_constant_block;

/* Read one fragment-program word from guest (PS3) memory: big-endian load
 * followed by a 16-bit half-word swap, yielding a host-order instruction
 * word. The decoder operates on host-order words. */
u32 rsx_fp_read_word(const u8* p);
u32 rsx_fp_sanitize_constant_word(u32 w);
extern unsigned long long g_rsx_fp_nonfinite_constants;

/* Decompile an NV40 fragment program into an HLSL pixel shader.
 *
 *   ucode     : pointer to the fragment-program bytecode in guest memory
 *               (big-endian, half-word-swapped — i.e. exactly as the RSX
 *               reads it; this function applies rsx_fp_read_word internally).
 *   max_bytes : safety bound on how far to read (program stops at PROGRAM_END
 *               or when this many bytes are consumed).
 *   out       : caller buffer for the generated HLSL (NUL-terminated).
 *   out_size  : size of `out` in bytes.
 *
 * Returns the number of instructions decoded (>=0) on success, or -1 on
 * error (null args, output overflow). The emitted entry point is
 * `float4 main(PSInput input) : SV_TARGET`, matching the backend's
 * placeholder PSInput { float4 position : SV_POSITION; float4 col : COLOR; }.
 *
 * Texture sampling and full input plumbing are partial — see the doc's
 * "Integration TODO". Unhandled opcodes emit a comment and a safe default so
 * the shader still compiles.
 *
 *   ctrl : the NV4097_SET_SHADER_CONTROL word (reg 0x1D60) for this program.
 *          Bit 0x40 (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) selects the
 *          fragment-output register set: clear => fp16 output (color = h0),
 *          set => fp32 output (color = r0). Pass RSX_FP_CTRL_AUTO when no
 *          control word is available (standalone tests) to fall back to the
 *          legacy "whichever of r0/h0 was written" heuristic. */
#define RSX_FP_CTRL_AUTO 0xFFFFFFFFu
int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size);

/* Apply the NV4097 fixed-function alpha test to an already-decompiled pixel
 * shader. D3D12 has no fixed-function alpha test, so the comparison is
 * emitted immediately before the shader's final color return. `func` uses
 * the CELL_GCM comparison values 0x200..0x207 and `ref` is normalized to the
 * render target's component representation. Returns 1 when a test was
 * inserted, 0 for ALWAYS, and -1 if the generated shader could not be
 * patched safely. */
int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref);

/* Decode NV4097_SET_ALPHA_REF for the active surface color format. Ordinary
 * integer render targets use the low 8 bits as UNORM8; W16Z16Y16X16 uses a
 * half float and X32/W32Z32Y32X32 use a 32-bit float. */
float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format);

/* Same as rsx_fp_decompile, plus per-texture-unit dimensionality.
 *
 *   tex_cube_mask : bit N set => texture unit N is a CUBEMAP. Cube units are
 *                   declared `TextureCube` and sampled with the full
 *                   3-component direction (a.xyz); 2D units keep `Texture2D`
 *                   and a.xy sampling. Pass 0 for the legacy all-2D behavior
 *                   (rsx_fp_decompile forwards 0, so its output is unchanged
 *                   byte-for-byte).
 *
 * The mask must match what the backend actually binds: a cube unit needs a
 * TextureCube SRV at register tN, and a 2D unit a Texture2D SRV. See the
 * fragment lane report (scratch/a010_reports/fragment.md) for replay_main.c
 * call-site guidance. */
int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size);

/* Buffered-constant variant of rsx_fp_decompile_ex. Inline CONST payloads are
 * represented as fp_constants[N] in a b1 PSConstants cbuffer rather than
 * printed as HLSL literals. `out_constant_count` receives the exact slot
 * count and may be NULL. */
int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count);

/* Extract the exact runtime inline-CONST payload. Returns the slot count on
 * success or -1 for an unterminated/truncated/oversized program. */
int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out);

/* Canonical structural fragment-program hash. Instruction bytes and their
 * order are hashed; inline CONST payload bytes are skipped. Returns zero for
 * malformed input. */
u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed);

/* Hash the exact legacy HLSL literal spellings (%g) for a program while also
 * retaining structural identity. This is intended for aggregate diagnostics;
 * PSO identity should use rsx_fp_structural_hash in buffered mode. */
u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed);

/* Buffered-alpha counterpart to rsx_fp_apply_alpha_test. It emits the compare
 * against fp_alpha.x from PSConstants instead of embedding a numeric value. */
int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func);

/* Plan one 256-byte-aligned allocation in a fence-retired PS constant ring.
 * Returns 1 when it fits, 0 when the caller must submit/wait/reset first, and
 * -1 when one allocation can never fit. */
int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes);

/* Plan one already-aligned fixed-size vertex-constant block.  The caller must
 * fence and retry from offset zero when this returns 0; silently dropping the
 * draw would leave a multi-buffered display surface on an older generation. */
int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset);

/* Return the mnemonic for an NV40 fragment opcode (or "?" if unknown).
 * Useful for disassembly/logging. */
const char* rsx_fp_opcode_name(u32 opcode);

/* Total byte length of the fragment program at `ucode` (including inline
 * CONST slots), up to and including the PROGRAM_END instruction, bounded by
 * `max_bytes`. Returns 0 if no terminator is found within the bound. Shared
 * walk logic so callers (e.g. a shader cache hashing the bytecode) agree with
 * the decompiler on where a program ends. */
u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes);

#ifdef __cplusplus
}
#endif

/* Kept from this tree (see the .c) -- used by rsx_d3d12_backend.c. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out);
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes);
#endif /* PS3RECOMP_RSX_FP_DECOMPILER_H */
