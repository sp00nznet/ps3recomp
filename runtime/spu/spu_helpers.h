/*
 * ps3recomp - SPU instruction semantics helpers
 *
 * Pure-C, header-only implementation of the per-instruction semantics used
 * by lifter-generated code. Extracted from spu_lifter.py so the helpers
 * have one source of truth and can be unit-tested directly (see
 * runtime/spu/tests/test_spu_helpers.c).
 *
 * Each helper is `static inline u128 spu_<mnemonic>(...)`. Naming, lane
 * widths and big-endian conventions match runtime/spu/spu_context.h.
 */

#ifndef SPU_HELPERS_H
#define SPU_HELPERS_H

#include "spu_context.h"
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#include <intrin.h>
static inline int spu_clz32(uint32_t x) {
    unsigned long idx;
    if (!x) return 32;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}
#else
static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
#endif

/* Architectural SPU barrier -- the lifted `sync`/`dsync`/`syncc` emission.
 * SPU ISA v1.2 §13 (+ the per-instruction pages): order all earlier LS and
 * channel accesses before later ones; LS must be consistent "if it were\n * observed by another entity" (Table 13-6 is exactly this runtime's
 * host-thread producer -> SPU consumer handoff). A no-op is neither a
 * compiler nor a CPU barrier -- MSVC at /O2 may reorder plain LS accesses
 * across an empty statement (the PPU twin of this bug is LESSONS #11d). A
 * full fence: the locked exchange forces total order and is opaque to the
 * optimizer. */
#ifdef _MSC_VER
static inline void spu_arch_fence(void) { long spu_fence_tmp = 0; _InterlockedExchange(&spu_fence_tmp, 1); }
#else
static inline void spu_arch_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

/* SPU byte position -> our _u8 index.
 * Our u128 is HOST-NATIVE little-endian (_u32[i]=SPU word i as a value via
 * spu_ls_read128's big-endian load), so within each 4-byte word the _u8[]
 * bytes are reversed vs SPU big-endian byte order. Any op defined in terms of
 * SPU *byte positions* (quadword byte rotates/shifts, gen-controls, shuffle
 * insertion) must map SPU byte P to our index W(P). Word-aligned operations
 * are unaffected (W is identity modulo the within-word reversal). */
#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))

#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
#include <arm_neon.h>
/* NEON forms of the hot integer ops. Each scalar definition stays as spu_X_ref
 * (tests/test_spu_int_neon.c compares them). */
static inline uint32x4_t spu__ld(u128 a) { return vld1q_u32(a._u32); }
static inline u128 spu__st(uint32x4_t v) { u128 r; vst1q_u32(r._u32, v); return r; }
/* Byte permute in SPU byte order: result SPU byte i = a's SPU byte I[i], or 0
 * when I[i] >= 16. SPU byte P is host byte P^3, so the host-order table index
 * is rev32(I) ^ 3 (out-of-range stays out of range). */
static inline u128 spu__perm(u128 a, uint8x16_t I) {
    uint8x16_t ih = veorq_u8(vrev32q_u8(I), vdupq_n_u8(3));
    u128 r; vst1q_u8(r._u8, vqtbl1q_u8(vld1q_u8(a._u8), ih)); return r;
}
static inline uint8x16_t spu__iota(void) {
    static const uint8_t k[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    return vld1q_u8(k);
}
#endif

/* Integer ops with a NEON form further down (end of file); the scalar definitions are
 * spu_X_ref, checked against the NEON forms by tests/test_spu_int_neon.c. */
static inline u128 spu_absdb(u128 a, u128 b);
static inline u128 spu_avgb(u128 a, u128 b);
static inline u128 spu_ceqb(u128 a, u128 b);
static inline u128 spu_cgtb(u128 a, u128 b);
static inline u128 spu_clgtb(u128 a, u128 b);
static inline u128 spu_ceqh(u128 a, u128 b);
static inline u128 spu_cgth(u128 a, u128 b);
static inline u128 spu_clgth(u128 a, u128 b);
static inline u128 spu_ah(u128 a, u128 b);
static inline u128 spu_sfh(u128 a, u128 b);
static inline u128 spu_shlh(u128 a, u128 b);
static inline u128 spu_roth(u128 a, u128 b);
static inline u128 spu_rothm(u128 a, u128 b);
static inline u128 spu_rothma(u128 a, u128 b);
static inline u128 spu_shl(u128 a, u128 b);
static inline u128 spu_rot(u128 a, u128 b);
static inline u128 spu_rotm(u128 a, u128 b);
static inline u128 spu_rotma(u128 a, u128 b);
static inline u128 spu_cg(u128 a, u128 b);
static inline u128 spu_bg(u128 a, u128 b);
static inline u128 spu_mpy(u128 a, u128 b);
static inline u128 spu_mpyu(u128 a, u128 b);
static inline u128 spu_mpyh(u128 a, u128 b);
static inline u128 spu_mpyhh(u128 a, u128 b);
static inline u128 spu_mpyhhu(u128 a, u128 b);
static inline u128 spu_mpys(u128 a, u128 b);
static inline u128 spu_sumb(u128 a, u128 b);
static inline u128 spu_ceqbi(u128 a, int32_t imm);
static inline u128 spu_cgtbi(u128 a, int32_t imm);
static inline u128 spu_clgtbi(u128 a, int32_t imm);
static inline u128 spu_ceqhi(u128 a, int32_t imm);
static inline u128 spu_cgthi(u128 a, int32_t imm);
static inline u128 spu_clgthi(u128 a, int32_t imm);
static inline u128 spu_ahi(u128 a, int32_t imm);
static inline u128 spu_sfhi(u128 a, int32_t imm);
static inline u128 spu_mpyi(u128 a, int32_t imm);
static inline u128 spu_mpyui(u128 a, int32_t imm);
static inline u128 spu_rothi(u128 a, int sh);
static inline u128 spu_roti(u128 a, int sh);
static inline u128 spu_rotmahi(u128 a, int i7);
static inline u128 spu_rotmai(u128 a, int i7);
static inline u128 spu_mpya(u128 a, u128 b, u128 c);
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t);
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t);
static inline u128 spu_addx(u128 a, u128 b, u128 t);
static inline u128 spu_sfx(u128 a, u128 b, u128 t);
static inline u128 spu_cgx(u128 a, u128 b, u128 t);
static inline u128 spu_bgx(u128 a, u128 b, u128 t);
static inline u128 spu_cntb(u128 a);
static inline u128 spu_xsbh(u128 a);
static inline u128 spu_xshw(u128 a);
static inline u128 spu_clz(u128 a);
static inline u128 spu_xswd(u128 a);
static inline u128 spu_orx(u128 a);
static inline u128 spu_gb(u128 a);
static inline u128 spu_gbh(u128 a);
static inline u128 spu_gbb(u128 a);
static inline u128 spu_fsm(u128 a);
static inline u128 spu_fsmh(u128 a);
static inline u128 spu_fsmb(u128 a);

/* ---- constructors ---- */
static inline u128 spu_splat_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=v; r._u32[2]=v; r._u32[3]=v; return r;
}
/* Link register value for brsl/bisl/bisled: the SPU writes the return address
 * into the PREFERRED word (word 0) and ZEROS the other three slots (matches
 * RPCS3 v128::from32r, SPUInterpreter.cpp BRSL). Splatting it (the old bug)
 * corrupts any code that saves/reloads/operates on the full 128-bit link. */
/* An SPU instruction the lifter could not translate. It emits a bare comment,
 * so an unsupported opcode on a live path silently does NOTHING and the guest
 * computes garbage from there on -- which surfaces much later as a wild DMA or
 * atomic address with no hint of where it came from. Most of these are data
 * embedded in .text that never executes; this fires only if one actually runs.
 * One line per site. */
static inline void spu_unsupported(uint32_t pc, const char* mnemonic)
{
    static int warned;
    if (warned < 64) {
        warned++;
        fprintf(stderr, "[spu] UNSUPPORTED %s at LS 0x%05X -- executed, "
                        "result is wrong from here\n", mnemonic, pc);
        fflush(stderr);
    }
}

static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
/* Preferred-slot scalar: value in word 0, remaining slots ZERO. CBEA's scalar
 * channel convention -- rchcnt returns the count this way (RPCS3 measured
 * {1,0,0,0} where the old splat gave {1,1,1,1}). Same bug class as spu_link. */
static inline u128 spu_pref_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_splat_u16(uint16_t v) {
    u128 r; for (int i=0;i<8;i++) r._u16[i]=v; return r;
}
static inline u128 spu_splat_u8(uint8_t v) {
    u128 r; for (int i=0;i<16;i++) r._u8[i]=v; return r;
}
static inline u128 spu_zero(void) { u128 r; memset(&r,0,sizeof r); return r; }

/* ---- integer arithmetic (SIMD) ---- */
static inline u128 spu_a_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_a(u128 a, u128 b) {
    return spu__st(vaddq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_a(u128 a, u128 b) { return spu_a_ref(a, b); }
#endif
static inline u128 spu_sf_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]-a._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_sf(u128 a, u128 b) {
    return spu__st(vsubq_u32(spu__ld(b), spu__ld(a)));
}
#else
static inline u128 spu_sf(u128 a, u128 b) { return spu_sf_ref(a, b); }
#endif
static inline u128 spu_ah_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh_ref(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ai_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ai(u128 a, int32_t imm) {
    return spu__st(vaddq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_ai(u128 a, int32_t imm) { return spu_ai_ref(a, imm); }
#endif
static inline u128 spu_ahi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)imm-a._u32[i]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_sfi(u128 a, int32_t imm) {
    return spu__st(vsubq_u32(vdupq_n_u32((uint32_t)imm), spu__ld(a)));
}
#else
static inline u128 spu_sfi(u128 a, int32_t imm) { return spu_sfi_ref(a, imm); }
#endif
static inline u128 spu_sfhi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }

/* ---- multiply (low halfword of each word × ... -> 32-bit, per SPU mpy) ----
 * Sub-lane indexing assumes a little-endian host (matches the recompiler's
 * target). The "low halfword of word i" in SPU BE semantics is _s16[2i] on
 * an LE host (NOT _s16[2i+1], which would be correct on a BE host). */
static inline u128 spu_mpy_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
/* mpya: 16x16 signed multiply of low halves + add rc (per word, RRR form). */
static inline u128 spu_mpya_ref(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
/* sfx: extended subtract rb-ra-1+carry; carry-in = low bit of old rt (RT is 3rd src). */
static inline u128 spu_sfx_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
static inline u128 spu_mpyu_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[i*2]*(uint32_t)b._u16[i*2]; return r; }
static inline u128 spu_mpyi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int16_t)imm; return r; }

/* ---- bitwise logic (whole 128 bits) ---- */
static inline u128 spu_and_ref(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]&b._u64[0]; r._u64[1]=a._u64[1]&b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_and(u128 a, u128 b) {
    return spu__st(vandq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_and(u128 a, u128 b) { return spu_and_ref(a, b); }
#endif
static inline u128 spu_or_ref(u128 a, u128 b)  { u128 r; r._u64[0]=a._u64[0]|b._u64[0]; r._u64[1]=a._u64[1]|b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_or(u128 a, u128 b) {
    return spu__st(vorrq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_or(u128 a, u128 b) { return spu_or_ref(a, b); }
#endif
static inline u128 spu_xor_ref(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]^b._u64[0]; r._u64[1]=a._u64[1]^b._u64[1]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_xor(u128 a, u128 b) {
    return spu__st(veorq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_xor(u128 a, u128 b) { return spu_xor_ref(a, b); }
#endif
static inline u128 spu_nand(u128 a, u128 b){ u128 r; r._u64[0]=~(a._u64[0]&b._u64[0]); r._u64[1]=~(a._u64[1]&b._u64[1]); return r; }
static inline u128 spu_nor(u128 a, u128 b) { u128 r; r._u64[0]=~(a._u64[0]|b._u64[0]); r._u64[1]=~(a._u64[1]|b._u64[1]); return r; }
static inline u128 spu_andc(u128 a, u128 b){ u128 r; r._u64[0]=a._u64[0]&~b._u64[0]; r._u64[1]=a._u64[1]&~b._u64[1]; return r; }
static inline u128 spu_orc(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]|~b._u64[0]; r._u64[1]=a._u64[1]|~b._u64[1]; return r; }
static inline u128 spu_andi_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]&(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_andi(u128 a, int32_t imm) {
    return spu__st(vandq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_andi(u128 a, int32_t imm) { return spu_andi_ref(a, imm); }
#endif
static inline u128 spu_ori_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ori(u128 a, int32_t imm) {
    return spu__st(vorrq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_ori(u128 a, int32_t imm) { return spu_ori_ref(a, imm); }
#endif
static inline u128 spu_xori_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]^(uint32_t)imm; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_xori(u128 a, int32_t imm) {
    return spu__st(veorq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_xori(u128 a, int32_t imm) { return spu_xori_ref(a, imm); }
#endif

/* ---- count leading zeros / population count per byte ---- */
static inline u128 spu_clz_ref(u128 a)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)spu_clz32(a._u32[i]); return r; }
static inline u128 spu_cntb_ref(u128 a) { u128 r; for(int i=0;i<16;i++){ uint8_t v=a._u8[i],c=0; while(v){c+=v&1;v>>=1;} r._u8[i]=c; } return r; }

/* ---- compares: all-ones / all-zeros per lane ---- */
static inline u128 spu_ceq_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]==b._u32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ceq(u128 a, u128 b) {
    return spu__st(vceqq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_ceq(u128 a, u128 b) { return spu_ceq_ref(a, b); }
#endif
static inline u128 spu_ceqh_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqb_ref(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgt_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>b._s32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cgt(u128 a, u128 b) {
    return spu__st(vcgtq_s32(vreinterpretq_s32_u32(spu__ld(a)), vreinterpretq_s32_u32(spu__ld(b))));
}
#else
static inline u128 spu_cgt(u128 a, u128 b) { return spu_cgt_ref(a, b); }
#endif
static inline u128 spu_cgth_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgtb_ref(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgt_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>b._u32[i])?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_clgt(u128 a, u128 b) {
    return spu__st(vcgtq_u32(spu__ld(a), spu__ld(b)));
}
#else
static inline u128 spu_clgt(u128 a, u128 b) { return spu_clgt_ref(a, b); }
#endif
static inline u128 spu_clgth_ref(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgtb_ref(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqi_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]==imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_ceqi(u128 a, int32_t imm) {
    return spu__st(vceqq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_ceqi(u128 a, int32_t imm) { return spu_ceqi_ref(a, imm); }
#endif
static inline u128 spu_cgti_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cgti(u128 a, int32_t imm) {
    return spu__st(vcgtq_s32(vreinterpretq_s32_u32(spu__ld(a)), vdupq_n_s32(imm)));
}
#else
static inline u128 spu_cgti(u128 a, int32_t imm) { return spu_cgti_ref(a, imm); }
#endif
static inline u128 spu_clgti_ref(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>(uint32_t)imm)?0xFFFFFFFFu:0; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_clgti(u128 a, int32_t imm) {
    return spu__st(vcgtq_u32(spu__ld(a), vdupq_n_u32((uint32_t)imm)));
}
#else
static inline u128 spu_clgti(u128 a, int32_t imm) { return spu_clgti_ref(a, imm); }
#endif

/* ---- select / shuffle ---- */
static inline u128 spu_selb_ref(u128 a, u128 b, u128 c) {
    u128 r;
    r._u64[0]=(a._u64[0]&~c._u64[0])|(b._u64[0]&c._u64[0]);
    r._u64[1]=(a._u64[1]&~c._u64[1])|(b._u64[1]&c._u64[1]);
    return r;
}
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    return spu__st(vbslq_u32(spu__ld(c), spu__ld(b), spu__ld(a)));
}
#else
static inline u128 spu_selb(u128 a, u128 b, u128 c) { return spu_selb_ref(a, b, c); }
#endif
/* shufb special selectors per Cell BE ISA:
 *   sel & 0xE0 == 0xE0 -> 0x80
 *   sel & 0xC0 == 0xC0 -> 0xFF
 *   sel & 0xC0 == 0x80 -> 0x00
 *   otherwise           -> concat{a,b}[sel & 0x1F] */
#if defined(__SSE4_1__)
#include <smmintrin.h>
/* pshufb version: one of the hottest SPU ops (~11% of GH3's FMOD mixer task
 * as the byte loop). SPU byte P lives at host byte SPU_W(P), i.e. the bytes
 * of each 32-bit word reversed, so swap into SPU order, select, swap back. */
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    const __m128i sw = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    __m128i A, B, C; memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    A = _mm_shuffle_epi8(A, sw); B = _mm_shuffle_epi8(B, sw); C = _mm_shuffle_epi8(C, sw);
    const __m128i idx = _mm_and_si128(C, _mm_set1_epi8(0x0F));
    const __m128i fromb = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), fromb);
    /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
    const __m128i m80 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0x80)), _mm_set1_epi8((char)0x80));
    const __m128i mC0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xC0)), _mm_set1_epi8((char)0xC0));
    const __m128i mE0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0xE0));
    const __m128i special = _mm_blendv_epi8(mC0, _mm_set1_epi8((char)0x80), mE0);
    r = _mm_shuffle_epi8(_mm_blendv_epi8(r, special, m80), sw);
    u128 out; memcpy(&out, &r, 16); return out;
}
#elif defined(__ARM_NEON)
#include <arm_neon.h>
/* NEON version: tbl over the 32-byte {a,b} concatenation. SPU byte P lives at
 * host byte P^3 (each word's bytes reversed), so rev32 into SPU order,
 * select, and rev32 back. */
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    uint8x16x2_t t;
    t.val[0] = vrev32q_u8(vld1q_u8(a._u8));
    t.val[1] = vrev32q_u8(vld1q_u8(b._u8));
    const uint8x16_t C = vrev32q_u8(vld1q_u8(c._u8));
    uint8x16_t r = vqtbl2q_u8(t, vandq_u8(C, vdupq_n_u8(0x1F)));
    /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
    const uint8x16_t m80 = vtstq_u8(C, vdupq_n_u8(0x80));
    const uint8x16_t mC0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xC0)), vdupq_n_u8(0xC0));
    const uint8x16_t mE0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xE0)), vdupq_n_u8(0xE0));
    const uint8x16_t special = vbslq_u8(mE0, vdupq_n_u8(0x80), mC0);
    r = vrev32q_u8(vbslq_u8(m80, special, r));
    u128 out; vst1q_u8(out._u8, r); return out;
}
#endif
#if defined(__SSE4_1__) || defined(__ARM_NEON)
/* The byte loop stays as the reference (tests/test_spu_shufb.c checks both). */
static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#else
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#define spu_shufb_ref spu_shufb
#endif

/* ---- shift / rotate immediate (word lanes) ---- */
static inline u128 spu_shli(u128 a, int sh)  { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_roti_ref(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi_ref(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }
static inline u128 spu_rotmai_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_shlqbyi_ref(u128 a, int sh) { u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqbyi(u128 a, int sh) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(sh & 0x1F))));
}
#else
static inline u128 spu_shlqbyi(u128 a, int sh) { return spu_shlqbyi_ref(a, sh); }
#endif
static inline u128 spu_rotqbyi_ref(u128 a, int sh) { u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqbyi(u128 a, int sh) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)sh)), vdupq_n_u8(15)));
}
#else
static inline u128 spu_rotqbyi(u128 a, int sh) { return spu_rotqbyi_ref(a, sh); }
#endif
/* Bit-level quadword shifts/rotates operate on the WHOLE 128-bit big-endian
 * value. Our _u64[0]/_u64[1] are word-SCRAMBLED (host-LE: _u64[0] = word0 |
 * word1<<32, but SPU's MS half is word0<<32 | word1), so native shifts on them
 * corrupt any bits crossing a 32-bit boundary. Assemble the logical hi/lo halves
 * with _u32[0] as most-significant, shift, then disassemble. */
static inline u128 spu_shlqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }

/* ---- SPU floating point --------------------------------------------------------------
 * Written from the Cell BE SPU ISA v1.2 (section 9 and the instruction pages).
 *
 * Single precision is "extended range" (section 9.1), not IEEE:
 *  - biased exponent 255 is an ordinary binade: no Inf, no NaN, and the largest magnitude is
 *    Smax = 0x7FFFFFFF = (2 - 2^-23) * 2^128;
 *  - biased exponent 0 is zero whatever the fraction (no denormals); any zero result is +0;
 *  - truncation (toward zero) is the only rounding mode;
 *  - a result above Smax saturates to +-Smax, one below Smin = 2^-126 becomes +0.
 * Each operation here takes its operands apart into sign, significand and power of two, computes
 * the exact result in integers (64/128-bit), and truncates once in spu__sf_pack. When one term is
 * so far below the other that it lies under the other's last bit, the exact sum is not formed:
 * its truncation is the dominant term's (same sign), or the dominant term less half its last bit
 * (opposite sign) -- which truncates to the same value as the exact result.
 *
 * Double precision (section 9.2) is IEEE with round-to-nearest (FPSCR rounding modes are not
 * modelled: fscrwr is ignored). Implementation choices the ISA leaves open, as the Cell makes
 * them: denormal operands are read as zero with their sign kept, and any NaN result is the
 * default QNaN 0x7FF8000000000000.
 * --------------------------------------------------------------------------------------- */

/* Exact value sign * sig * 2^e2 (sig > 0) as an SPU single: keep the top 24 bits of sig
 * (truncation), then saturate above Smax / flush below Smin. */
static inline uint32_t spu__sf_pack(uint32_t sign, unsigned __int128 sig, int e2) {
    const uint64_t hi = (uint64_t)(sig >> 64), lo = (uint64_t)sig;
    const int top = hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll(lo);   /* sig in [2^top, 2^(top+1)) */
    const int biased = top + e2 + 127;
    if (biased > 255) return sign | 0x7FFFFFFFu;
    if (biased < 1)   return 0;
    const uint32_t m24 = top >= 23 ? (uint32_t)(sig >> (top - 23)) : (uint32_t)(sig << (23 - top));
    return sign | ((uint32_t)biased << 23) | (m24 & 0x7FFFFFu);
}
/* An SPU single as sig * 2^e2 with a 24-bit sig; returns 0 for zero (biased exponent 0). */
static inline int spu__sf_split(uint32_t x, uint32_t* sig, int* e2) {
    const uint32_t be = (x >> 23) & 0xFFu;
    if (!be) return 0;
    *sig = (x & 0x7FFFFFu) | 0x800000u;
    *e2 = (int)be - 150;                                   /* -127 bias, -23 for the integer sig */
    return 1;
}
/* Exact sum of two signed terms s1*m1*2^e1 + s2*m2*2^e2 (m1, m2 < 2^48, both nonzero), truncated. */
static inline uint32_t spu__sf_sum(uint32_t s1, uint64_t m1, int e1, uint32_t s2, uint64_t m2, int e2) {
    if (e1 < e2) { uint32_t ts = s1; s1 = s2; s2 = ts; uint64_t tm = m1; m1 = m2; m2 = tm; int te = e1; e1 = e2; e2 = te; }
    const int d = e1 - e2;                                 /* term 1 has the higher exponent */
    if (d > 79) {                                          /* term 2 < 2^(e1-31): below term 1's last bit */
        if (s1 == s2) return spu__sf_pack(s1, m1, e1);
        return spu__sf_pack(s1, ((unsigned __int128)m1 << 1) - 1, e1 - 1);
    }
    const unsigned __int128 a = (unsigned __int128)m1 << d, b = m2;   /* both at 2^e2, < 2^127 */
    if (s1 == s2) return spu__sf_pack(s1, a + b, e2);
    if (a == b) return 0;
    return a > b ? spu__sf_pack(s1, a - b, e2) : spu__sf_pack(s2, b - a, e2);
}
static inline uint32_t spu__fa_lane(uint32_t x, uint32_t y) {
    uint32_t mx, my; int ex, ey;
    const int nx = spu__sf_split(x, &mx, &ex), ny = spu__sf_split(y, &my, &ey);
    if (!nx) return ny ? y : 0;                            /* a nonzero single is already exact */
    if (!ny) return x;
    return spu__sf_sum(x & 0x80000000u, mx, ex, y & 0x80000000u, my, ey);
}
static inline uint32_t spu__fm_lane(uint32_t x, uint32_t y) {
    uint32_t mx, my; int ex, ey;
    if (!spu__sf_split(x, &mx, &ex) || !spu__sf_split(y, &my, &ey)) return 0;
    return spu__sf_pack((x ^ y) & 0x80000000u, (uint64_t)mx * my, ex + ey);
}
/* (+-a*b) + (+-c), the product exact and unbounded (fma page), one truncation. */
static inline uint32_t spu__fma_lane(uint32_t x, uint32_t y, uint32_t z, uint32_t negp, uint32_t negc) {
    uint32_t mx, my, mz; int ex, ey, ez;
    const int np = spu__sf_split(x, &mx, &ex) & spu__sf_split(y, &my, &ey);
    const int nz = spu__sf_split(z, &mz, &ez);
    const uint32_t sp = ((x ^ y) & 0x80000000u) ^ negp, sz = (z & 0x80000000u) ^ negc;
    if (!np) return nz ? (sz | (z & 0x7FFFFFFFu)) : 0;
    const uint64_t p = (uint64_t)mx * my;
    if (!nz) return spu__sf_pack(sp, p, ex + ey);
    return spu__sf_sum(sp, p, ex + ey, sz, mz, ez);
}
/* Fast path, exact by construction, for the common case where every operand is an ordinary
 * normal single (biased exponent 1..254): the value is formed in double precision and truncated
 * by masking the 29 low significand bits, which is truncation toward zero of the magnitude.
 *  - a product of two 24-bit significands is exact in double;
 *  - a sum is exact in double when the exponents differ by at most 29;
 *  - x*y + z through a double fma is rounded once at 53 bits; that rounding can only change the
 *    24-bit truncation when the double lands exactly on a 24-bit boundary (its 29 low bits all
 *    zero), so such lanes take the exact path.
 * Anything else -- zero, extended, or a result outside the normal range -- returns 0 here,
 * meaning "use the exact path". */
static inline int spu__sf_plain(uint32_t x) { const uint32_t e = (x >> 23) & 0xFFu; return e - 1u < 254u; }
static inline float spu__sf_f(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
/* d (finite, nonzero) truncated to a single; 0 if outside the normal single range or zero. */
static inline uint32_t spu__sf_from_d(double d, int boundary_check) {
    uint64_t u; memcpy(&u, &d, 8);
    const int e = (int)((u >> 52) & 0x7FFu) - 1023;
    if (e < -126 || e > 127) return 0;                     /* zero, denormal, or not a normal single */
    if (boundary_check && !(u & 0x1FFFFFFFull)) return 0;  /* possibly rounded onto the boundary */
    return (uint32_t)(u >> 32 & 0x80000000u) | ((uint32_t)(e + 127) << 23) | (uint32_t)((u >> 29) & 0x7FFFFFu);
}
static inline uint32_t spu__fa_fast(uint32_t x, uint32_t y) {
    if (!spu__sf_plain(x) || !spu__sf_plain(y)) return 0;
    const int d = (int)((x >> 23) & 0xFFu) - (int)((y >> 23) & 0xFFu);
    if (d > 29 || d < -29) return 0;
    return spu__sf_from_d((double)spu__sf_f(x) + (double)spu__sf_f(y), 0);
}
static inline uint32_t spu__fm_fast(uint32_t x, uint32_t y) {
    if (!spu__sf_plain(x) || !spu__sf_plain(y)) return 0;
    return spu__sf_from_d((double)spu__sf_f(x) * (double)spu__sf_f(y), 0);
}
static inline uint32_t spu__fma_fast(uint32_t x, uint32_t y, uint32_t z, uint32_t negp, uint32_t negc) {
    if (!spu__sf_plain(x) || !spu__sf_plain(y) || !spu__sf_plain(z)) return 0;
    const double p = (double)spu__sf_f(x ^ negp) * (double)spu__sf_f(y);      /* exact */
    return spu__sf_from_d(fma(1.0, p, (double)spu__sf_f(z ^ negc)), 1);
}
static inline uint32_t spu__fa_any(uint32_t x, uint32_t y) { const uint32_t f = spu__fa_fast(x, y); return f ? f : spu__fa_lane(x, y); }
static inline uint32_t spu__fm_any(uint32_t x, uint32_t y) { const uint32_t f = spu__fm_fast(x, y); return f ? f : spu__fm_lane(x, y); }
static inline uint32_t spu__fma_any(uint32_t x, uint32_t y, uint32_t z, uint32_t np, uint32_t nc) {
    const uint32_t f = spu__fma_fast(x, y, z, np, nc); return f ? f : spu__fma_lane(x, y, z, np, nc); }
#define SPU__SF_V2(name, expr) static inline u128 name(u128 a, u128 b) { u128 r; \
    for (int i = 0; i < 4; i++) { const uint32_t x = a._u32[i], y = b._u32[i]; r._u32[i] = (expr); } return r; }
#define SPU__SF_V3(name, expr) static inline u128 name(u128 a, u128 b, u128 c) { u128 r; \
    for (int i = 0; i < 4; i++) { const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i]; r._u32[i] = (expr); } return r; }
/* Per-lane forms (fast path, else exact). */
SPU__SF_V2(spu__fa_s, spu__fa_any(x, y))
SPU__SF_V2(spu__fs_s, spu__fa_any(x, y ^ 0x80000000u))
SPU__SF_V2(spu__fm_s, spu__fm_any(x, y))
SPU__SF_V3(spu__fma_s,  spu__fma_any(x, y, z, 0, 0))
SPU__SF_V3(spu__fms_s,  spu__fma_any(x, y, z, 0, 0x80000000u))
SPU__SF_V3(spu__fnms_s, spu__fma_any(x, y, z, 0x80000000u, 0))
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
/* The same fast path four lanes at a time, as two f64x2 halves: widen (exact), operate, clear
 * the 29 low significand bits, narrow (exact: the value now fits a single). Each lane is judged
 * on its own: the lanes that qualify keep the vector result, and only the others are redone one
 * by one (zero is not "plain", so a vector with a 0.0 lane -- w = 0, padding -- is common).
 *
 * Inlining: the fast path is forced inline and the per-lane fix-up is kept out of line (noinline,
 * cold). Left to itself the compiler inlined the scalar fallback into spu__nv_fa/fma, found the
 * result too big, and called the whole helper out of line instead -- every fa/fm/fma became a
 * call with the operands moved through general registers (tests/bench/spu: fa 13.4 ns, fma
 * 27.8 ns per op, latency). */
#define SPU__NV_INLINE static inline __attribute__((always_inline))
#define SPU__NV_SLOW   static inline __attribute__((noinline, cold))
SPU__NV_INLINE uint32x4_t spu__nv_bexp(uint32x4_t x) { return vshrq_n_u32(vshlq_n_u32(x, 1), 24); }
SPU__NV_INLINE uint32x4_t spu__nv_plain(uint32x4_t x) {         /* biased exponent 1..254 */
    return vcltq_u32(vsubq_u32(spu__nv_bexp(x), vdupq_n_u32(1)), vdupq_n_u32(254));
}
/* Truncate f64x2 to singles. *ok gets all-ones in the lanes whose result is a normal single and,
 * when err is given (the exact rounding error of the operation that produced d), was not both
 * rounded and on a 24-bit boundary: only there can rounding have crossed the boundary and
 * changed the truncation. */
SPU__NV_INLINE float32x2_t spu__nv_trunc(float64x2_t d, uint32x2_t* ok, const float64x2_t* err) {
    const uint64x2_t u = vreinterpretq_u64_f64(d);
    const int64x2_t e = vsubq_s64(vreinterpretq_s64_u64(vandq_u64(vshrq_n_u64(u, 52), vdupq_n_u64(0x7FF))), vdupq_n_s64(1023));
    uint64x2_t good = vandq_u64(vcgeq_s64(e, vdupq_n_s64(-126)), vcleq_s64(e, vdupq_n_s64(127)));
    if (err) good = vandq_u64(good, vorrq_u64(vtstq_u64(u, vdupq_n_u64(0x1FFFFFFFull)), vceqzq_f64(*err)));
    *ok = vmovn_u64(good);
    return vcvt_f32_f64(vreinterpretq_f64_u64(vandq_u64(u, vdupq_n_u64(~0x1FFFFFFFull))));
}
/* As spu__nv_trunc, but an exact zero result is accepted too; *zm marks those lanes so the caller
 * can clear the sign (the SPU never produces -0). Used once true zeros (+-0.0 words) are fast-path
 * operands: in double, 0 + y = y, x * 0 = +-0, 0 * y + z = z exactly -- the SPU's zero rules --
 * and a zero result of nonzero operands is an exact cancellation, which the SPU returns as +0.
 * The sign mask is computed beside the conversion, so it costs one AND on the result path. */
SPU__NV_INLINE float32x2_t spu__nv_trunc0(float64x2_t d, uint32x2_t* ok, uint32x2_t* zm, const float64x2_t* err) {
    const uint64x2_t u = vreinterpretq_u64_f64(d);
    const uint64x2_t az = vceqzq_u64(vshlq_n_u64(u, 1));                  /* +-0 */
    const int64x2_t e = vsubq_s64(vreinterpretq_s64_u64(vandq_u64(vshrq_n_u64(u, 52), vdupq_n_u64(0x7FF))), vdupq_n_s64(1023));
    uint64x2_t good = vandq_u64(vcgeq_s64(e, vdupq_n_s64(-126)), vcleq_s64(e, vdupq_n_s64(127)));
    if (err) good = vandq_u64(good, vorrq_u64(vtstq_u64(u, vdupq_n_u64(0x1FFFFFFFull)), vceqzq_f64(*err)));
    *ok = vmovn_u64(vorrq_u64(good, az));
    *zm = vmovn_u64(az);
    return vcvt_f32_f64(vreinterpretq_f64_u64(vandq_u64(u, vdupq_n_u64(~0x1FFFFFFFull))));
}
/* A lane the fast path takes: an ordinary single (biased exponent 1..254) or a true zero
 * (+-0.0). Exponent-0 words with a nonzero fraction are zero on the SPU but not in double, so
 * they still go to the fix-up. */
SPU__NV_INLINE uint32x4_t spu__nv_plain0(uint32x4_t x) {
    return vorrq_u32(spu__nv_plain(x), vceqzq_u32(vshlq_n_u32(x, 1)));
}
/* r with the sign cleared in the lanes whose result is zero */
SPU__NV_INLINE uint32x4_t spu__nv_pos0(uint32x4_t r, uint32x2_t zlo, uint32x2_t zhi) {
    return vbicq_u32(r, vandq_u32(vcombine_u32(zlo, zhi), vdupq_n_u32(0x80000000u)));
}
SPU__NV_INLINE int spu__nv_all4(uint32x4_t m) { return vminvq_u32(m) == 0xFFFFFFFFu; }
SPU__NV_INLINE float64x2_t spu__nv_lo(uint32x4_t x) { return vcvt_f64_f32(vget_low_f32(vreinterpretq_f32_u32(x))); }
SPU__NV_INLINE float64x2_t spu__nv_hi(uint32x4_t x) { return vcvt_high_f64_f32(vreinterpretq_f32_u32(x)); }
/* The lanes of r not marked in ok, recomputed one by one (scalar fast path, else exact). They take
 * and return NEON vectors, not u128: a u128 travels in general registers, and merging that return
 * with the fast path's vector result kept every fa/fm/fma result in x-registers, with fmov round
 * trips, on the fast path too (fa 5.3 -> 9.8 ns). */
#define SPU__NV_LANES(v, m) uint32_t m[4]; vst1q_u32(m, v)
SPU__NV_INLINE uint32x4_t spu__nv_vec(float32x2_t lo, float32x2_t hi) { return vreinterpretq_u32_f32(vcombine_f32(lo, hi)); }
SPU__NV_INLINE uint32x4_t spu__nv_zero(uint32x4_t x) { return vceqzq_u32(spu__nv_bexp(x)); }   /* biased exponent 0: zero */
/* SPU_FLOATFIX_STATS=1: count why lanes reach the fix-ups (T-0014) and print the tally every
 * 2^24 lanes. Only the cold fix-ups count, so it costs nothing on the fast path; the counters
 * are one weak global shared by every TU that includes this header. */
enum { SPU_FF_ZX, SPU_FF_ZY, SPU_FF_EXT, SPU_FF_GAP, SPU_FF_RANGE, SPU_FF_BOUNDARY, SPU_FF_N };
__attribute__((weak)) uint64_t spu_floatfix_stats[3][SPU_FF_N];
__attribute__((weak)) uint64_t spu_floatfix_vectors[3];
__attribute__((weak)) int spu_floatfix_on = -1;
__attribute__((weak)) uint64_t spu_floatfix_lanes;
static inline __attribute__((cold, noinline)) void spu__ff_count(int op, int why) {
    static const char* const ops[3] = { "fa/fs", "fm", "fma/fms/fnms" };
    __atomic_fetch_add(&spu_floatfix_stats[op][why], 1, __ATOMIC_RELAXED);
    if ((__atomic_add_fetch(&spu_floatfix_lanes, 1, __ATOMIC_RELAXED) & 0xFFFFFF) != 0) return;
    uint64_t n = 0;
    for (int o = 0; o < 3; o++) for (int k = 0; k < SPU_FF_N; k++) n += __atomic_load_n(&spu_floatfix_stats[o][k], __ATOMIC_RELAXED);
    char buf[640]; int p = snprintf(buf, sizeof buf, "[spu-floatfix] %llu lanes:", (unsigned long long)n);
    for (int o = 0; o < 3; o++)
        p += snprintf(buf + p, sizeof buf - p, "  %s vec=%llu zero-a=%llu zero-b=%llu ext=%llu gap=%llu range=%llu boundary=%llu;",
                      ops[o], (unsigned long long)spu_floatfix_vectors[o],
                      (unsigned long long)spu_floatfix_stats[o][SPU_FF_ZX], (unsigned long long)spu_floatfix_stats[o][SPU_FF_ZY],
                      (unsigned long long)spu_floatfix_stats[o][SPU_FF_EXT], (unsigned long long)spu_floatfix_stats[o][SPU_FF_GAP],
                      (unsigned long long)spu_floatfix_stats[o][SPU_FF_RANGE], (unsigned long long)spu_floatfix_stats[o][SPU_FF_BOUNDARY]);
    fprintf(stderr, "%s\n", buf);
}
static inline int spu__ff_enabled(void) {
    int on = __atomic_load_n(&spu_floatfix_on, __ATOMIC_RELAXED);
    if (on < 0) { const char* e = getenv("SPU_FLOATFIX_STATS"); on = (e && e[0] == '1');
                  __atomic_store_n(&spu_floatfix_on, on, __ATOMIC_RELAXED); }
    return on;
}
static inline uint32_t spu__ff_be(uint32_t v) { return (v >> 23) & 0xFFu; }
static inline int spu__ff_why_fa(uint32_t x, uint32_t y);
static inline int spu__ff_why_fm(uint32_t x, uint32_t y);
static inline int spu__ff_why_fma(uint32_t x, uint32_t y, uint32_t z, uint32_t negp, uint32_t negc);
static inline __attribute__((cold, noinline)) void spu__ff_tally(int op, uint32x4_t ok, uint32x4_t a, uint32x4_t b,
                                                                  uint32x4_t c, uint32_t negp, uint32_t negc) {
    SPU__NV_LANES(ok, m); SPU__NV_LANES(a, x); SPU__NV_LANES(b, y); SPU__NV_LANES(c, z);
    __atomic_fetch_add(&spu_floatfix_vectors[op], 1, __ATOMIC_RELAXED);
    for (int i = 0; i < 4; i++) if (!m[i])
        spu__ff_count(op, op == 0 ? spu__ff_why_fa(x[i], y[i]) : op == 1 ? spu__ff_why_fm(x[i], y[i])
                                  : spu__ff_why_fma(x[i], y[i], z[i], negp, negc));
}
/* Why a fa/fs lane failed (y already sign-adjusted). zero-a/zero-b: operand 1/2 is zero;
 * ext: an operand is extended range; gap: exponents differ by more than 29; range: the
 * result is not a normal single. */
static inline int spu__ff_why_fa(uint32_t x, uint32_t y) {
    const int bx = (int)spu__ff_be(x), by = (int)spu__ff_be(y);
    if (!bx) return SPU_FF_ZX;
    if (!by) return SPU_FF_ZY;
    if (bx == 255 || by == 255) return SPU_FF_EXT;
    if (bx - by > 29 || by - bx > 29) return SPU_FF_GAP;
    return SPU_FF_RANGE;
}
static inline int spu__ff_why_fm(uint32_t x, uint32_t y) {
    const int bx = (int)spu__ff_be(x), by = (int)spu__ff_be(y);
    if (!bx) return SPU_FF_ZX;
    if (!by) return SPU_FF_ZY;
    if (bx == 255 || by == 255) return SPU_FF_EXT;
    return SPU_FF_RANGE;
}
/* fma: zero-a = the product is zero (x or y), zero-b = the addend is zero; boundary = the
 * TwoSum test (rounded onto a 24-bit boundary), told apart from range by redoing the sum. */
static inline int spu__ff_why_fma(uint32_t x, uint32_t y, uint32_t z, uint32_t negp, uint32_t negc) {
    const int bx = (int)spu__ff_be(x), by = (int)spu__ff_be(y), bz = (int)spu__ff_be(z);
    if (!bx || !by) return SPU_FF_ZX;
    if (!bz) return SPU_FF_ZY;
    if (bx == 255 || by == 255 || bz == 255) return SPU_FF_EXT;
    const double s = (double)spu__sf_f(x ^ negp) * (double)spu__sf_f(y) + (double)spu__sf_f(z ^ negc);
    uint64_t u; memcpy(&u, &s, 8);
    const int e = (int)((u >> 52) & 0x7FFu) - 1023;
    return (e < -126 || e > 127) ? SPU_FF_RANGE : SPU_FF_BOUNDARY;
}
SPU__NV_SLOW uint32x4_t spu__fa_fix(uint32x4_t r, uint32x4_t ok, uint32x4_t a, uint32x4_t b, uint32_t negb) {
    if (spu__ff_enabled()) spu__ff_tally(0, ok, a, veorq_u32(b, vdupq_n_u32(negb)), b, 0, 0);
    SPU__NV_LANES(r, o); SPU__NV_LANES(ok, m); SPU__NV_LANES(a, x); SPU__NV_LANES(b, y);
    for (int i = 0; i < 4; i++) if (!m[i]) o[i] = spu__fa_any(x[i], y[i] ^ negb);
    return vld1q_u32(o);
}
SPU__NV_SLOW uint32x4_t spu__fm_fix(uint32x4_t r, uint32x4_t ok, uint32x4_t a, uint32x4_t b) {
    if (spu__ff_enabled()) spu__ff_tally(1, ok, a, b, b, 0, 0);
    SPU__NV_LANES(r, o); SPU__NV_LANES(ok, m); SPU__NV_LANES(a, x); SPU__NV_LANES(b, y);
    for (int i = 0; i < 4; i++) if (!m[i]) o[i] = spu__fm_any(x[i], y[i]);
    return vld1q_u32(o);
}
SPU__NV_SLOW uint32x4_t spu__fma_fix(uint32x4_t r, uint32x4_t ok, uint32x4_t a, uint32x4_t b, uint32x4_t c,
                                     uint32_t negp, uint32_t negc) {
    if (spu__ff_enabled()) spu__ff_tally(2, ok, a, b, c, negp, negc);
    SPU__NV_LANES(r, o); SPU__NV_LANES(ok, m); SPU__NV_LANES(a, x); SPU__NV_LANES(b, y); SPU__NV_LANES(c, z);
    for (int i = 0; i < 4; i++) if (!m[i]) o[i] = spu__fma_any(x[i], y[i], z[i], negp, negc);
    return vld1q_u32(o);
}
SPU__NV_INLINE u128 spu__nv_u128(uint32x4_t v) { u128 r; vst1q_u32(r._u32, v); return r; }
SPU__NV_INLINE u128 spu__nv_fa(u128 a, u128 b, uint32_t negb) {
    const uint32x4_t xa = vld1q_u32(a._u32), yb = vld1q_u32(b._u32);
    const uint32x4_t x = xa, y = veorq_u32(yb, vdupq_n_u32(negb));
    const uint32x4_t px = spu__nv_plain0(x), py = spu__nv_plain0(y);
    /* the exponent-gap limit applies only between two nonzero terms */
    const uint32x4_t pre = vandq_u32(vandq_u32(px, py),
                                     vorrq_u32(vcleq_u32(vabdq_u32(spu__nv_bexp(x), spu__nv_bexp(y)), vdupq_n_u32(29)),
                                               vorrq_u32(vceqzq_u32(vshlq_n_u32(x, 1)), vceqzq_u32(vshlq_n_u32(y, 1)))));
    uint32x2_t oklo, okhi, zlo, zhi;
    const float32x2_t lo = spu__nv_trunc0(vaddq_f64(spu__nv_lo(x), spu__nv_lo(y)), &oklo, &zlo, NULL);
    const float32x2_t hi = spu__nv_trunc0(vaddq_f64(spu__nv_hi(x), spu__nv_hi(y)), &okhi, &zhi, NULL);
    const uint32x4_t ok = vandq_u32(pre, vcombine_u32(oklo, okhi));
    uint32x4_t r = spu__nv_pos0(spu__nv_vec(lo, hi), zlo, zhi);
    if (!spu__nv_all4(ok)) r = spu__fa_fix(r, ok, xa, yb, negb);
    return spu__nv_u128(r);
}
SPU__NV_INLINE u128 spu_fa(u128 a, u128 b) { return spu__nv_fa(a, b, 0); }
SPU__NV_INLINE u128 spu_fs(u128 a, u128 b) { return spu__nv_fa(a, b, 0x80000000u); }
SPU__NV_INLINE u128 spu_fm(u128 a, u128 b) {
    const uint32x4_t x = vld1q_u32(a._u32), y = vld1q_u32(b._u32);
    uint32x2_t oklo, okhi, zlo, zhi;
    const float32x2_t lo = spu__nv_trunc0(vmulq_f64(spu__nv_lo(x), spu__nv_lo(y)), &oklo, &zlo, NULL);
    const float32x2_t hi = spu__nv_trunc0(vmulq_f64(spu__nv_hi(x), spu__nv_hi(y)), &okhi, &zhi, NULL);
    const uint32x4_t ok = vandq_u32(vandq_u32(spu__nv_plain0(x), spu__nv_plain0(y)), vcombine_u32(oklo, okhi));
    uint32x4_t r = spu__nv_pos0(spu__nv_vec(lo, hi), zlo, zhi);
    if (!spu__nv_all4(ok)) r = spu__fm_fix(r, ok, x, y);
    return spu__nv_u128(r);
}
/* p + z in double with its exact rounding error (Knuth's TwoSum: round to nearest, no overflow
 * -- the operands here are at most 2^257). *err == 0 means the sum was exact. */
SPU__NV_INLINE float64x2_t spu__nv_twosum(float64x2_t p, float64x2_t z, float64x2_t* err) {
    const float64x2_t s = vaddq_f64(p, z), bb = vsubq_f64(s, p);
    *err = vaddq_f64(vsubq_f64(p, vsubq_f64(s, bb)), vsubq_f64(z, bb));
    return s;
}
/* (+-a*b) + (+-c): the product is exact in double; one rounding in the add, and TwoSum says
 * whether it rounded. (The old test sent every result with 29 zero low bits to the exact path --
 * every exactly representable result, 1.5*1 + 0.5 -- so fma's common case was slow.) */
SPU__NV_INLINE u128 spu__nv_fma(u128 a, u128 b, u128 c, uint32_t negp, uint32_t negc) {
    const uint32x4_t xa = vld1q_u32(a._u32), y = vld1q_u32(b._u32), zc = vld1q_u32(c._u32);
    const uint32x4_t x = veorq_u32(xa, vdupq_n_u32(negp)), z = veorq_u32(zc, vdupq_n_u32(negc));
    uint32x2_t oklo, okhi, zlo, zhi;
    float64x2_t elo, ehi;
    const float64x2_t slo = spu__nv_twosum(vmulq_f64(spu__nv_lo(x), spu__nv_lo(y)), spu__nv_lo(z), &elo);
    const float64x2_t shi = spu__nv_twosum(vmulq_f64(spu__nv_hi(x), spu__nv_hi(y)), spu__nv_hi(z), &ehi);
    const float32x2_t lo = spu__nv_trunc0(slo, &oklo, &zlo, &elo);
    const float32x2_t hi = spu__nv_trunc0(shi, &okhi, &zhi, &ehi);
    const uint32x4_t ok = vandq_u32(vandq_u32(vandq_u32(spu__nv_plain0(x), spu__nv_plain0(y)), spu__nv_plain0(z)),
                                    vcombine_u32(oklo, okhi));
    uint32x4_t r = spu__nv_pos0(spu__nv_vec(lo, hi), zlo, zhi);
    if (!spu__nv_all4(ok)) r = spu__fma_fix(r, ok, xa, y, zc, negp, negc);
    return spu__nv_u128(r);
}
SPU__NV_INLINE u128 spu_fma(u128 a, u128 b, u128 c)  { return spu__nv_fma(a, b, c, 0, 0); }            /* a*b + c */
SPU__NV_INLINE u128 spu_fms(u128 a, u128 b, u128 c)  { return spu__nv_fma(a, b, c, 0, 0x80000000u); }  /* a*b - c */
SPU__NV_INLINE u128 spu_fnms(u128 a, u128 b, u128 c) { return spu__nv_fma(a, b, c, 0x80000000u, 0); }  /* c - a*b */
#else
static inline u128 spu_fa(u128 a, u128 b) { return spu__fa_s(a, b); }
static inline u128 spu_fs(u128 a, u128 b) { return spu__fs_s(a, b); }
static inline u128 spu_fm(u128 a, u128 b) { return spu__fm_s(a, b); }
static inline u128 spu_fma(u128 a, u128 b, u128 c)  { return spu__fma_s(a, b, c); }
static inline u128 spu_fms(u128 a, u128 b, u128 c)  { return spu__fms_s(a, b, c); }
static inline u128 spu_fnms(u128 a, u128 b, u128 c) { return spu__fnms_s(a, b, c); }
#endif
/* The exact path alone, for tests that check the fast path against it. */
SPU__SF_V2(spu_fa_ref, spu__fa_lane(x, y))
SPU__SF_V2(spu_fs_ref, spu__fa_lane(x, y ^ 0x80000000u))
SPU__SF_V2(spu_fm_ref, spu__fm_lane(x, y))
SPU__SF_V3(spu_fma_ref,  spu__fma_lane(x, y, z, 0, 0))
SPU__SF_V3(spu_fms_ref,  spu__fma_lane(x, y, z, 0, 0x80000000u))
SPU__SF_V3(spu_fnms_ref, spu__fma_lane(x, y, z, 0x80000000u, 0))
/* Compares (always extended range): any two zeros are equal and never greater. Nonzero
 * magnitudes order like their bit patterns. */
static inline int64_t spu__sf_key(uint32_t x) {
    if (!(x & 0x7F800000u)) return 0;
    const int64_t m = x & 0x7FFFFFFFu;
    return (x & 0x80000000u) ? -m : m;
}
static inline int64_t spu__sf_mag(uint32_t x) { return (x & 0x7F800000u) ? (int64_t)(x & 0x7FFFFFFFu) : 0; }
SPU__SF_V2(spu_fceq,  spu__sf_key(x) == spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcgt,  spu__sf_key(x) >  spu__sf_key(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcmeq, spu__sf_mag(x) == spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
SPU__SF_V2(spu_fcmgt, spu__sf_mag(x) >  spu__sf_mag(y) ? 0xFFFFFFFFu : 0u)
#undef SPU__SF_V2
#undef SPU__SF_V3

/* cflts / cfltu: value * 2^(173 - i8), truncated toward zero, saturating to the integer range.
 * csflt / cuflt: integer / 2^(155 - i8) as an extended-range single (truncated). The ISA leaves
 * scales outside 0..127 undefined; these compute the same formula for them. */
static inline uint32_t spu__cfl_lane(uint32_t x, int scale, int is_unsigned) {
    uint32_t m; int e;
    if (!spu__sf_split(x, &m, &e)) return 0;
    const int sh = e + scale, neg = (x >> 31) != 0;
    if (is_unsigned) {
        if (neg) return 0;
        if (sh >= 9) return 0xFFFFFFFFu;                   /* >= 2^32 */
        return sh >= 0 ? m << sh : (sh <= -24 ? 0 : m >> -sh);
    }
    if (sh >= 8) return neg ? 0x80000000u : 0x7FFFFFFFu;   /* magnitude >= 2^31 */
    const uint32_t v = sh >= 0 ? m << sh : (sh <= -24 ? 0 : m >> -sh);
    return neg ? 0u - v : v;
}
static inline u128 spu_cflts(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = spu__cfl_lane(a._u32[i], 173 - (i8 & 0xFF), 0); return r; }
static inline u128 spu_cfltu(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = spu__cfl_lane(a._u32[i], 173 - (i8 & 0xFF), 1); return r; }
static inline u128 spu_csflt(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) { const int32_t v = a._s32[i];
    r._u32[i] = v ? spu__sf_pack(v < 0 ? 0x80000000u : 0, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, -(155 - (i8 & 0xFF))) : 0; } return r; }
static inline u128 spu_cuflt(u128 a, int i8) { u128 r; for (int i = 0; i < 4; i++) { const uint32_t v = a._u32[i];
    r._u32[i] = v ? spu__sf_pack(0, v, -(155 - (i8 & 0xFF))) : 0; } return r; }

/* fi -- Floating Interpolate (SPU ISA v1.2, p. 219; frest/frsqest output
 * format p. 215; single-precision rules section 9.1).
 *
 * rb holds an estimate as produced by frest/frsqest: sign S, biased exponent E,
 * a 13-bit BaseFraction and a 10-bit StepFraction. ra supplies Y = 0.ra[13:31]
 * (its low 19 bits, binary point above them). The result is
 *
 *     (-1)^S * (1.BaseFraction - 0.000StepFraction * Y) * 2^(E - 127)
 *
 * 1.BaseFraction is (2^13 + base) * 2^-13 and 0.000StepFraction is
 * step * 2^-13, so with Y = y * 2^-19 every term is an integer multiple of
 * 2^-32: the subtraction is exact in 64-bit fixed point. The magnitude lies in
 * (7/8, 2), so it needs at most one left shift to normalise; the fraction is
 * then truncated to 23 bits (truncation is the only SPU rounding mode). An
 * exponent-0 estimate is a zero operand, and a result whose exponent would
 * drop below 1 underflows -- both give +0, the only zero the SPU produces.
 * Exponent 255 is ordinary extended range, so 1/0 stays at the top of it. */
static inline u128 spu_fi(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        const uint32_t est = b._u32[i];
        const uint32_t sign = est & 0x80000000u;
        int exp = (int)((est >> 23) & 0xFFu);
        const uint64_t base = (est >> 10) & 0x1FFFu;          /* BaseFraction, 13 bits */
        const uint64_t step = est & 0x3FFu;                   /* StepFraction, 10 bits */
        const uint64_t y    = a._u32[i] & 0x7FFFFu;           /* Y numerator, 19 bits */
        if (exp == 0) { r._u32[i] = 0; continue; }
        /* magnitude * 2^32: (2^13 + base) * 2^19 - step * y */
        uint64_t m = ((((uint64_t)1 << 13) | base) << 19) - step * y;
        if (m < ((uint64_t)1 << 32)) {                        /* below 1.0: normalise */
            m <<= 1;
            if (--exp == 0) { r._u32[i] = 0; continue; }      /* underflow -> +0 */
        }
        const uint32_t frac = (uint32_t)((m >> 9) & 0x7FFFFFu); /* drop the hidden 1, truncate */
        r._u32[i] = sign | ((uint32_t)exp << 23) | frac;
    }
    return r;
}

/* ---- double precision: IEEE, round to nearest (see the header of this section) ---- */
static inline uint64_t spu__dw_get(u128 r, int i) { return ((uint64_t)r._u32[2*i] << 32) | r._u32[2*i+1]; }
static inline void spu__dw_put(u128* r, int i, uint64_t u) { r->_u32[2*i] = (uint32_t)(u >> 32); r->_u32[2*i+1] = (uint32_t)u; }
static inline int spu__d_isnan(uint64_t u) { return (u & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
/* operand: a denormal reads as a zero of the same sign */
static inline double spu__d_in(uint64_t u) {
    if (!(u & 0x7FF0000000000000ull)) u &= 0x8000000000000000ull;
    double d; memcpy(&d, &u, 8); return d;
}
static inline uint64_t spu__d_out(double d) {
    uint64_t u; memcpy(&u, &d, 8);
    return spu__d_isnan(u) ? 0x7FF8000000000000ull : u;
}
#define SPU__D_V2(name, op) static inline u128 name(u128 a, u128 b) { u128 r; for (int i = 0; i < 2; i++) { \
    const uint64_t x = spu__dw_get(a, i), y = spu__dw_get(b, i); \
    spu__dw_put(&r, i, (spu__d_isnan(x) || spu__d_isnan(y)) ? 0x7FF8000000000000ull \
                       : spu__d_out(spu__d_in(x) op spu__d_in(y))); } return r; }
SPU__D_V2(spu_dfa, +)
SPU__D_V2(spu_dfs, -)
SPU__D_V2(spu_dfm, *)
#undef SPU__D_V2
/* dfma = a*b + c, dfms = a*b - c, dfnms = -(a*b - c), dfnma = -(a*b + c); c is rt. Fused. */
static inline u128 spu__dfmx(u128 a, u128 b, u128 c, int negate, int subtract) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint64_t x = spu__dw_get(a, i), y = spu__dw_get(b, i), z = spu__dw_get(c, i);
        if (spu__d_isnan(x) || spu__d_isnan(y) || spu__d_isnan(z)) { spu__dw_put(&r, i, 0x7FF8000000000000ull); continue; }
        const double zz = subtract ? -spu__d_in(z) : spu__d_in(z);
        const double v = fma(spu__d_in(x), spu__d_in(y), zz);
        /* Negate the RESULT by its sign bit: written as -fma(..) the compiler may fold it into a
         * negated fused multiply-add, (-x*y) - z, which differs from -(x*y + z) in the sign of an
         * exact zero. */
        const uint64_t u = spu__d_out(v);
        spu__dw_put(&r, i, (negate && !spu__d_isnan(u)) ? u ^ 0x8000000000000000ull : u);
    }
    return r; }
static inline u128 spu_dfma(u128 a, u128 b, u128 t)  { return spu__dfmx(a, b, t, 0, 0); }
static inline u128 spu_dfms(u128 a, u128 b, u128 t)  { return spu__dfmx(a, b, t, 0, 1); }
static inline u128 spu_dfnms(u128 a, u128 b, u128 t) { return spu__dfmx(a, b, t, 1, 1); }
static inline u128 spu_dfnma(u128 a, u128 b, u128 t) { return spu__dfmx(a, b, t, 1, 0); }
/* fesd: the single in word 2i (IEEE format, section 9.2.1) extended to doubleword i: exact,
 * Inf stays Inf, a NaN becomes the default QNaN, a denormal becomes +0.
 * frds: doubleword i rounded (to nearest) to an IEEE single in word 2i, word 2i+1 = 0; a NaN
 * becomes the single default QNaN 0x7FC00000. */
static inline u128 spu_fesd(u128 a) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint32_t w = a._u32[2*i], be = (w >> 23) & 0xFFu, f = w & 0x7FFFFFu;
        const uint64_t sign = (uint64_t)(w >> 31) << 63;
        uint64_t u;
        if (be == 0)        u = f ? 0 : sign;
        else if (be == 255) u = f ? 0x7FF8000000000000ull : (sign | 0x7FF0000000000000ull);
        else                u = sign | ((uint64_t)(be - 127 + 1023) << 52) | ((uint64_t)f << 29);
        spu__dw_put(&r, i, u);
    }
    return r; }
static inline u128 spu_frds(u128 a) { u128 r;
    for (int i = 0; i < 2; i++) {
        const uint64_t u = spu__dw_get(a, i);
        if (spu__d_isnan(u)) { r._u32[2*i] = 0x7FC00000u; r._u32[2*i+1] = 0; continue; }
        const float f = (float)spu__d_in(u);
        uint32_t w; memcpy(&w, &f, 4);
        r._u32[2*i] = w; r._u32[2*i+1] = 0;
    }
    return r; }


/* ---- SPU double-precision (2 doubles/reg; dword i = words 2i(high),2i+1(low)).
 * Reassemble via _u32 -- our u128 is host-LE, so a naive _f64[i] would SWAP the
 * two words. Semantics from RPCS3 SPUInterpreter (DFASM / DFMA / FESD / FRDS). ---- */
static inline double spu__dget(u128 r, int i) {
    uint64_t u = ((uint64_t)r._u32[i*2] << 32) | r._u32[i*2+1];
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline void spu__dset(u128* r, int i, double d) {
    uint64_t u; memcpy(&u, &d, sizeof u);
    r->_u32[i*2] = (uint32_t)(u >> 32); r->_u32[i*2+1] = (uint32_t)u;
}
/* double FMA family: c = rt (accumulator, 3-register). The SPU DFMA family is FUSED (single rounding). */
/* double compares -> per-lane 64-bit mask (RPCS3 stubs these; sane impl here). */
static inline u128 spu_dfceq(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* Singles occupy preferred words 0 and 2 when converting doubleword lanes. */
/* mpyhhu: high-16 x high-16 of each word, unsigned, full 32-bit product. */
static inline u128 spu_mpyhhu_ref(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* cgx: extended carry-generate, carry-in = low bit of old rt (RPCS3 CGX). */
static inline u128 spu_cgx_ref(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

/* ---- remaining SPU ISA ops (RPCS3 SPUInterpreter: EQV/ABSDB/AVGB/MPYHHA/
 * MPYHHAU/DFCGT/DFCMGT/XORBI/DFTSV). Completes the lifter's opcode coverage. ---- */
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
/* mpyhha/mpyhhau: high-16 x high-16, ACCUMULATE into rt (3-register). */
static inline u128 spu_mpyhha_ref(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* double compare greater (RPCS3 stubs these; sane impl) -> per-lane mask. */
static inline u128 spu_dfcgt(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* dftsv rt,ra,I7: double test special value (Cell BE SPU ISA). Each
 * doubleword becomes all ones if it is in any class I7 selects:
 * 0x40 NaN, 0x20 +inf, 0x10 -inf, 0x08 +0, 0x04 -0, 0x02 +denorm, 0x01 -denorm.
 * (RPCS3 stubs it as fatal; this is the ISA definition.) */
static inline uint64_t spu__dftsv1(uint64_t v, uint32_t i7) {
    uint64_t e = (v >> 52) & 0x7FF, f = v & 0xFFFFFFFFFFFFFull; int neg = (int)(v >> 63);
    int t = 0;
    if (e == 0x7FF && f) t |= i7 & 0x40;
    if (e == 0x7FF && !f) t |= i7 & (neg ? 0x10 : 0x20);
    if (e == 0 && !f) t |= i7 & (neg ? 0x04 : 0x08);
    if (e == 0 && f) t |= i7 & (neg ? 0x01 : 0x02);
    return t ? ~0ull : 0ull;
}
static inline u128 spu_dftsv(u128 a, int32_t imm){ u128 r;
    for (int i = 0; i < 2; i++) { uint64_t v = (uint64_t)a._u32[2*i] << 32 | a._u32[2*i+1];
        uint64_t m = spu__dftsv1(v, (uint32_t)imm & 0x7F); r._u32[2*i] = (uint32_t)(m >> 32); r._u32[2*i+1] = (uint32_t)m; }
    return r; }

/* ---- Phase 2: register-variable shifts/rotates ---- */
static inline u128 spu_shl_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); } return r; }
static inline u128 spu_shlh_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); } return r; }
static inline u128 spu_rot_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&31; r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; } return r; }
static inline u128 spu_roth_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&15; r._u16[i]= sh ? (uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))) : a._u16[i]; } return r; }
static inline u128 spu_shlqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_shlqby_ref(u128 a, u128 b){ int sh=b._u32[0]&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqby(u128 a, u128 b) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(b._u32[0] & 0x1F))));
}
#else
static inline u128 spu_shlqby(u128 a, u128 b) { return spu_shlqby_ref(a, b); }
#endif
static inline u128 spu_rotqby_ref(u128 a, u128 b){ int sh=b._u32[0]&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqby(u128 a, u128 b) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)b._u32[0])), vdupq_n_u8(15)));
}
#else
static inline u128 spu_rotqby(u128 a, u128 b) { return spu_rotqby_ref(a, b); }
#endif
static inline u128 spu_shlqbybi_ref(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_shlqbybi(u128 a, u128 b) {
    return spu__perm(a, vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)((b._u32[0] >> 3) & 0x1F))));
}
#else
static inline u128 spu_shlqbybi(u128 a, u128 b) { return spu_shlqbybi_ref(a, b); }
#endif
static inline u128 spu_rotqbybi_ref(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqbybi(u128 a, u128 b) {
    return spu__perm(a, vandq_u8(vaddq_u8(spu__iota(), vdupq_n_u8((uint8_t)(b._u32[0] >> 3))), vdupq_n_u8(15)));
}
#else
static inline u128 spu_rotqbybi(u128 a, u128 b) { return spu_rotqbybi_ref(a, b); }
#endif

/* ---- Phase 2: rotmahi ---- */
static inline u128 spu_rotmahi_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

/* ---- Phase 2: byte/half immediate compares ---- */
static inline u128 spu_ceqbi_ref(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi_ref(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi_ref(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi_ref(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi_ref(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi_ref(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

/* ---- Phase 2: misc one-offs ---- */
static inline u128 spu_fscrrd(u128 a) { (void)a; return spu_zero(); }
static inline u128 spu_gb_ref(u128 a) {
    uint32_t v = ((a._u32[0]&1)<<3)|((a._u32[1]&1)<<2)|((a._u32[2]&1)<<1)|(a._u32[3]&1);
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_gbh_ref(u128 a) {
    /* gather LSB of each SPU halfword H into bit (7-H). SPU halfword H maps to
     * our _u16[H^1] (the within-word halfword swap of the value layout). */
    uint32_t v=0; for(int i=0;i<8;i++) v |= ((uint32_t)(a._u16[i^1]&1) << (7-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
/* gather LSB of each SPU byte i into bit (15-i); exact inverse of spu_fsmb.
 * SPU byte i lives at our _u8[SPU_W(i)] (byte-reversed within each word). */
static inline u128 spu_gbb_ref(u128 a) {
    uint32_t v=0; for(int i=0;i<16;i++) v |= ((uint32_t)(a._u8[SPU_W(i)]&1) << (15-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_cg_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
/* sumb: sum the 4 bytes of each word. Per CBEA, RT halfword 2i (the HIGH half of
 * word i) = sum of RB's 4 bytes of word i; halfword 2i+1 (LOW half) = sum of RA's
 * 4 bytes of word i. Computed on word VALUES so byte order is irrelevant. */
static inline u128 spu_sumb_ref(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
/* Borrow generate: carry-out of (b + ~a + 1) == (b >= a unsigned ? 1 : 0).
 * The subtract-side sibling of cg; pairs with sf/sfx for extended subtraction. */
static inline u128 spu_bg_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx_ref(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
/* LE host: high half of word i = _s16[2i+1], low half = _s16[2i]. */
static inline u128 spu_mpyh_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._s32[i]=((int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i]) << 16; return r; }
static inline u128 spu_mpyhh_ref(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpys_ref(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++){ int32_t p=(int32_t)a._s16[2*i]*(int32_t)b._s16[2*i]; r._s32[i]=(int16_t)(p>>16); } return r; }
static inline u128 spu_mpyui_ref(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i]*(uint32_t)(uint16_t)imm; return r; }
static const uint32_t spu_frest_fraction_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};

static const uint32_t spu_frsqest_fraction_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};

static inline u128 spu_frsqest(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x3Fu;
        uint32_t rexp = (exp == 0u) ? 0xFFu : (190u - (exp + 1u) / 2u);
        uint32_t fraction = spu_frsqest_fraction_lut[frac_idx];
        uint32_t out = (rexp << 23) | (fraction & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}
/* frest: reciprocal estimate (refined by the following spu_fi Newton step).
 * Full-precision 1/x is exact after fi and >= HW-estimate accuracy. */
static inline u128 spu_frest(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t sign = bits & 0x80000000u;
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x1Fu;
        /* result biased exponent = 253 - exp (0 for exp >= 253); exp 0 (zero/denormal) gives 255. Fraction from the
         * hardware table. Matches RPCS3's measured spu_frest_{fraction,exponent}_lut for every input. */
        uint32_t rexp = (exp == 0u) ? 255u : (exp >= 253u) ? 0u : (253u - exp);
        uint32_t out = sign | (rexp << 23) | (spu_frest_fraction_lut[frac_idx] & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

/* ---- Phase 3: sign extension ----
 * LE host: low sub-lane = _u8[2i] / _s16[2i] / _s32[2i] (the byte/half/word
 * at the *lower* storage address). Same caveat as the mpy family. */
static inline u128 spu_xsbh_ref(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw_ref(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
/* xswd: rt.doubleword[d] = sign_extend_64(ra.word[2d+1]) -- the ODD word of
 * each doubleword, because that is the low half in big-endian order.
 *
 * This read the EVEN words (2d) and wrote the result through _s64, which is a
 * host little-endian lane: writing it puts the value in _u32[2d] (SPU word 2d,
 * the HIGH half) and the sign fill in _u32[2d+1]. Both halves were therefore
 * wrong, and the common case -- a small positive value -- came out as ZERO.
 *
 * You Don't Know Jack's FMOD mixer hit exactly that: a compiler-generated
 * 64-bit multiply helper received `1 * 0` instead of `1 * 16`, so the mixer's
 * block count was zero and it stopped on its own assert. Unlike its siblings
 * xsbh/xshw, whose source and destination swaps cancel, nothing cancels here.
 * Write the two words explicitly. */
static inline u128 spu_xswd_ref(u128 a) {
    u128 r;
    for (int d = 0; d < 2; d++) {
        int32_t v = (int32_t)a._u32[2*d + 1];
        r._u32[2*d]     = (uint32_t)(v >> 31);   /* sign fill (high word) */
        r._u32[2*d + 1] = (uint32_t)v;           /* value      (low word) */
    }
    return r;
}

/* ---- Phase 3: OR across ---- */
static inline u128 spu_orx_ref(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

/* ---- Phase 3: form-select mask from bits ---- */
static inline u128 spu_fsm_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xF;
    for(int i=0;i<4;i++) r._u32[i] = ((v>>(3-i))&1) ? 0xFFFFFFFFu : 0;
    return r;
}
/* fsmh: per-halfword mask from 8 bits. SPU halfword H <- bit (7-H); store at
 * our _u16[H^1] (within-word halfword swap of the value layout). Per-halfword
 * both bytes are identical so byte order within a halfword is irrelevant, but
 * the halfword ORDER within each word must be swapped. */
static inline u128 spu_fsmh_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFF;
    for(int i=0;i<8;i++) r._u16[i^1] = ((v>>(7-i))&1) ? 0xFFFFu : 0;
    return r;
}
/* fsmb/fsmbi: per-byte mask from 16 bits. SPU byte position P <- bit (15-P);
 * store at our _u8[SPU_W(P)] (byte-reversed within each word). A bit set at SPU
 * byte P must land at our _u8[SPU_W(P)] so that downstream word arithmetic (e.g.
 * andbi(fsmbi(0x101),-128) building a +0x80 EA offset in the low byte of words
 * 1,3) places the value in the correct byte lane. Raw _u8[P] would put 0x80 in
 * the high byte (bit 31) instead -> wrong DMA EA. */
static inline u128 spu_fsmb_ref(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}
static inline u128 spu_fsmbi(int32_t imm) {
    u128 r; uint32_t v = imm & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}

/* ---- Phase 3: constant generators (insertion shuffle patterns) ---- */
/* Generate-controls (cbd/chd/cwd/cdd) — insertion selectors for the (now
 * SPU-byte-correct) shufb. Each builds a control in TRUE SPU byte order: the
 * base selects b's SPU byte t at result SPU byte t (selector 0x10+t), and the
 * insert positions select a's preferred scalar bytes -- SPU byte 3 for a byte
 * (selector 0x03), SPU bytes 2,3 for a halfword (0x02,0x03), SPU bytes 0..3 for
 * a word, 0..7 for a doubleword. Every store is mapped through SPU_W to land at
 * the right host index. Verified to compose correctly with the fixed shufb for
 * both cbd-generated and immediate/LS-constant controls. */
static inline u128 spu_cbd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03;                 /* a's preferred byte = SPU byte 3 */
    return r;
}
static inline u128 spu_chd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1;                       /* SPU halfword byte positions p, p+1 */
    r._u8[SPU_W(p)]   = 0x02;                        /* a SPU byte 2 (hi byte of preferred hw) */
    r._u8[SPU_W(p+1)] = 0x03;                        /* a SPU byte 3 (lo byte) */
    return r;
}
static inline u128 spu_cwd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3;
    for(int k=0;k<4;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..3 (preferred word) */
    return r;
}
static inline u128 spu_cdd_pos(int pos) {
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7;
    for(int k=0;k<8;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..7 (preferred dword) */
    return r;
}
static inline u128 spu_cbd(u128 a, int i7){ return spu_cbd_pos((int)a._u32[0]+i7); }
static inline u128 spu_chd(u128 a, int i7){ return spu_chd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cwd_ref(u128 a, int i7){ return spu_cwd_pos((int)a._u32[0]+i7); }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_cwd(u128 a, int i7) {
    static const uint32_t base[4] = {0x10111213u, 0x14151617u, 0x18191A1Bu, 0x1C1D1E1Fu}, lane[4] = {0, 1, 2, 3};
    uint32x4_t m = vceqq_u32(vld1q_u32(lane), vdupq_n_u32((((uint32_t)a._u32[0] + (uint32_t)i7) >> 2) & 3));
    return spu__st(vbslq_u32(m, vdupq_n_u32(0x00010203u), vld1q_u32(base)));
}
#else
static inline u128 spu_cwd(u128 a, int i7) { return spu_cwd_ref(a, i7); }
#endif
static inline u128 spu_cdd(u128 a, int i7){ return spu_cdd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cbx(u128 a, u128 b){ return spu_cbd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_chx(u128 a, u128 b){ return spu_chd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cwx(u128 a, u128 b){ return spu_cwd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cdx(u128 a, u128 b){ return spu_cdd_pos((int)(a._u32[0]+b._u32[0])); }

/* ---- Phase 3: rotate-and-mask family ---- */
static inline u128 spu_rotm_ref(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); } return r; }
static inline u128 spu_rotma_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); } return r; }
static inline u128 spu_rothm_ref(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); } return r; }
static inline u128 spu_rothma_ref(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); } return r; }
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rotqmbi(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmby_ref(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmby(u128 a, u128 b) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - (int)b._u32[0]) & 0x1F))));
}
#else
static inline u128 spu_rotqmby(u128 a, u128 b) { return spu_rotqmby_ref(a, b); }
#endif
static inline u128 spu_rotqmbybi_ref(u128 a, u128 b) { int sh=(0-((int)b._u32[0]>>3))&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmbybi(u128 a, u128 b) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - ((int)b._u32[0] >> 3)) & 0x1F))));
}
#else
static inline u128 spu_rotqmbybi(u128 a, u128 b) { return spu_rotqmbybi_ref(a, b); }
#endif
static inline u128 spu_rotqmbii(u128 a, int i7)  { int sh=(0-i7)&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmbyi_ref(u128 a, int i7)  { int sh=(0-i7)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline u128 spu_rotqmbyi(u128 a, int i7) {
    return spu__perm(a, vsubq_u8(spu__iota(), vdupq_n_u8((uint8_t)((0 - i7) & 0x1F))));
}
#else
static inline u128 spu_rotqmbyi(u128 a, int i7) { return spu_rotqmbyi_ref(a, i7); }
#endif

/* ---- Phase 3: halfword/byte immediate logic ---- */
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }

/* ---- Phase 3: borrow-generate extended ---- */
static inline u128 spu_bgx_ref(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

/* ---- Phase 3: mfspr stub ---- */
static inline u128 spu_mfspr(u128 a) { (void)a; return spu_zero(); }

/* ---- immediate loaders ---- */
static inline u128 spu_il(int32_t imm)   { return spu_splat_u32((uint32_t)imm); }
static inline u128 spu_ila(uint32_t i18) { return spu_splat_u32(i18 & 0x3FFFF); }
static inline u128 spu_ilh(uint16_t imm) { return spu_splat_u16(imm); }
static inline u128 spu_ilhu(uint16_t imm){ return spu_splat_u32((uint32_t)imm << 16); }
static inline u128 spu_iohl(u128 a, uint16_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }

#ifdef __cplusplus
}
#endif

/* ---- NEON forms of the per-lane integer ops ------------------------------------------
 * Each was a scalar loop over 4/8/16 lanes (tests/bench/spu: 3-20 ns per op lifted); here it
 * is one to five vector instructions. Bit-identical to spu_X_ref (tests/test_spu_int_neon.c). */
#if defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS) && !defined(SPU_SCALAR_INT)
static inline uint16x8_t vnegq_u16_compat(uint16x8_t x) { return vsubq_u16(vdupq_n_u16(0), x); }
static inline u128 spu_absdb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vabdq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_avgb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vrhaddq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_ceqb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vceqq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_cgtb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_s8(vreinterpretq_s8_u32(spu__ld(a)), vreinterpretq_s8_u32(spu__ld(b)))));
}
static inline u128 spu_clgtb(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_u8(vreinterpretq_u8_u32(spu__ld(a)), vreinterpretq_u8_u32(spu__ld(b)))));
}
static inline u128 spu_ceqh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vceqq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_cgth(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_s16(vreinterpretq_s16_u32(spu__ld(a)), vreinterpretq_s16_u32(spu__ld(b)))));
}
static inline u128 spu_clgth(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_ah(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vaddq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_u16_u32(spu__ld(b)))));
}
static inline u128 spu_sfh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vsubq_u16(vreinterpretq_u16_u32(spu__ld(b)), vreinterpretq_u16_u32(spu__ld(a)))));
}
static inline u128 spu_shlh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_u16(vshlq_u16(vreinterpretq_u16_u32(spu__ld(a)), vreinterpretq_s16_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x001F001Fu))))));
}
static inline u128 spu_roth(u128 a, u128 b) {
    const uint16x8_t x = vreinterpretq_u16_u32(spu__ld(a)); const int16x8_t n = vreinterpretq_s16_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x000F000Fu)));
    return spu__st(vreinterpretq_u32_u16(vorrq_u16(vshlq_u16(x, n), vshlq_u16(x, vsubq_s16(n, vdupq_n_s16(16))))));
}
static inline u128 spu_rothm(u128 a, u128 b) {
    const int16x8_t m = vnegq_s16(vreinterpretq_s16_u16(vandq_u16(vnegq_u16_compat(vreinterpretq_u16_u32(spu__ld(b))), vdupq_n_u16(0x1F))));
    return spu__st(vreinterpretq_u32_u16(vshlq_u16(vreinterpretq_u16_u32(spu__ld(a)), m)));
}
static inline u128 spu_rothma(u128 a, u128 b) {
    const int16x8_t m = vnegq_s16(vreinterpretq_s16_u16(vandq_u16(vnegq_u16_compat(vreinterpretq_u16_u32(spu__ld(b))), vdupq_n_u16(0x1F))));
    return spu__st(vreinterpretq_u32_s16(vshlq_s16(vreinterpretq_s16_u32(spu__ld(a)), m)));
}
static inline u128 spu_shl(u128 a, u128 b) {
    return spu__st(vshlq_u32(spu__ld(a), vreinterpretq_s32_u32(vandq_u32(spu__ld(b), vdupq_n_u32(0x3F)))));
}
static inline u128 spu_rot(u128 a, u128 b) {
    const uint32x4_t x = spu__ld(a); const int32x4_t n = vreinterpretq_s32_u32(vandq_u32(spu__ld(b), vdupq_n_u32(31)));
    return spu__st(vorrq_u32(vshlq_u32(x, n), vshlq_u32(x, vsubq_s32(n, vdupq_n_s32(32)))));
}
static inline u128 spu_rotm(u128 a, u128 b) {
    const int32x4_t m = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), spu__ld(b)), vdupq_n_u32(0x3F))));
    return spu__st(vshlq_u32(spu__ld(a), m));
}
static inline u128 spu_rotma(u128 a, u128 b) {
    const int32x4_t m = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(vsubq_u32(vdupq_n_u32(0), spu__ld(b)), vdupq_n_u32(0x3F))));
    return spu__st(vreinterpretq_u32_s32(vshlq_s32(vreinterpretq_s32_u32(spu__ld(a)), m)));
}
static inline u128 spu_cg(u128 a, u128 b) {
    const uint32x4_t x = spu__ld(a);
    return spu__st(vshrq_n_u32(vcltq_u32(vaddq_u32(x, spu__ld(b)), x), 31));
}
static inline u128 spu_bg(u128 a, u128 b) {
    return spu__st(vshrq_n_u32(vcgeq_u32(spu__ld(b), spu__ld(a)), 31));
}
static inline u128 spu_mpy(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16))));
}
static inline u128 spu_mpyu(u128 a, u128 b) {
    const uint32x4_t m = vdupq_n_u32(0xFFFFu);
    return spu__st(vmulq_u32(vandq_u32(spu__ld(a), m), vandq_u32(spu__ld(b), m)));
}
static inline u128 spu_mpyh(u128 a, u128 b) {
    return spu__st(vshlq_n_u32(vmulq_u32(vshrq_n_u32(spu__ld(a), 16), spu__ld(b)), 16));
}
static inline u128 spu_mpyhh(u128 a, u128 b) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16))));
}
static inline u128 spu_mpyhhu(u128 a, u128 b) {
    return spu__st(vmulq_u32(vshrq_n_u32(spu__ld(a), 16), vshrq_n_u32(spu__ld(b), 16)));
}
static inline u128 spu_mpys(u128 a, u128 b) {
    const int32x4_t p = vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16));
    return spu__st(vreinterpretq_u32_s32(vshrq_n_s32(p, 16)));
}
static inline u128 spu_sumb(u128 a, u128 b) {
    const uint32x4_t sa = vpaddlq_u16(vpaddlq_u8(vreinterpretq_u8_u32(spu__ld(a)))), sb = vpaddlq_u16(vpaddlq_u8(vreinterpretq_u8_u32(spu__ld(b))));
    return spu__st(vorrq_u32(vshlq_n_u32(sb, 16), sa));
}
static inline u128 spu_ceqbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vceqq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8((uint8_t)imm))));
}
static inline u128 spu_cgtbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_s8(vreinterpretq_s8_u32(spu__ld(a)), vdupq_n_s8((int8_t)imm))));
}
static inline u128 spu_clgtbi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u8(vcgtq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8((uint8_t)imm))));
}
static inline u128 spu_ceqhi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vceqq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)imm))));
}
static inline u128 spu_cgthi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)imm))));
}
static inline u128 spu_clgthi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vcgtq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16((uint16_t)imm))));
}
static inline u128 spu_ahi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vaddq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16((uint16_t)imm))));
}
static inline u128 spu_sfhi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_u16(vsubq_u16(vdupq_n_u16((uint16_t)imm), vreinterpretq_u16_u32(spu__ld(a)))));
}
static inline u128 spu_mpyi(u128 a, int32_t imm) {
    return spu__st(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vdupq_n_s32((int16_t)imm))));
}
static inline u128 spu_mpyui(u128 a, int32_t imm) {
    return spu__st(vmulq_u32(vandq_u32(spu__ld(a), vdupq_n_u32(0xFFFFu)), vdupq_n_u32((uint16_t)imm)));
}
static inline u128 spu_rothi(u128 a, int sh) {
    const uint16x8_t x = vreinterpretq_u16_u32(spu__ld(a)); sh &= 15;
    return spu__st(vreinterpretq_u32_u16(vorrq_u16(vshlq_u16(x, vdupq_n_s16((int16_t)sh)), vshlq_u16(x, vdupq_n_s16((int16_t)(sh - 16))))));
}
static inline u128 spu_roti(u128 a, int sh) {
    const uint32x4_t x = spu__ld(a); sh &= 31;
    return spu__st(vorrq_u32(vshlq_u32(x, vdupq_n_s32(sh)), vshlq_u32(x, vdupq_n_s32(sh - 32))));
}
static inline u128 spu_rotmahi(u128 a, int i7) {
    return spu__st(vreinterpretq_u32_s16(vshlq_s16(vreinterpretq_s16_u32(spu__ld(a)), vdupq_n_s16((int16_t)-((0 - i7) & 0x1F)))));
}
static inline u128 spu_rotmai(u128 a, int i7) {
    return spu__st(vreinterpretq_u32_s32(vshlq_s32(vreinterpretq_s32_u32(spu__ld(a)), vdupq_n_s32(-((0 - i7) & 0x3F)))));
}
static inline u128 spu_mpya(u128 a, u128 b, u128 c) {
    return spu__st(vaddq_u32(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16), vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16), 16))), spu__ld(c)));
}
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vreinterpretq_u32_s32(vmulq_s32(vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), vshrq_n_s32(vreinterpretq_s32_u32(spu__ld(b)), 16))), spu__ld(t)));
}
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) {
    return spu__st(vmlaq_u32(spu__ld(t), vshrq_n_u32(spu__ld(a), 16), vshrq_n_u32(spu__ld(b), 16)));
}
static inline u128 spu_addx(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vaddq_u32(spu__ld(a), spu__ld(b)), vandq_u32(spu__ld(t), vdupq_n_u32(1))));
}
static inline u128 spu_sfx(u128 a, u128 b, u128 t) {
    return spu__st(vaddq_u32(vaddq_u32(spu__ld(b), vmvnq_u32(spu__ld(a))), vandq_u32(spu__ld(t), vdupq_n_u32(1))));
}
static inline u128 spu_cgx(u128 a, u128 b, u128 t) {
    const uint32x4_t x = spu__ld(a), s = vaddq_u32(x, spu__ld(b)), s2 = vaddq_u32(s, vandq_u32(spu__ld(t), vdupq_n_u32(1)));
    return spu__st(vshrq_n_u32(vorrq_u32(vcltq_u32(s, x), vcltq_u32(s2, s)), 31));
}
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    const uint32x4_t y = spu__ld(b), s = vaddq_u32(y, vmvnq_u32(spu__ld(a))), s2 = vaddq_u32(s, vandq_u32(spu__ld(t), vdupq_n_u32(1)));
    return spu__st(vshrq_n_u32(vorrq_u32(vcltq_u32(s, y), vcltq_u32(s2, s)), 31));
}
static inline u128 spu_cntb(u128 a) {
    return spu__st(vreinterpretq_u32_u8(vcntq_u8(vreinterpretq_u8_u32(spu__ld(a)))));
}
static inline u128 spu_xsbh(u128 a) {
    return spu__st(vreinterpretq_u32_s16(vshrq_n_s16(vshlq_n_s16(vreinterpretq_s16_u32(spu__ld(a)), 8), 8)));
}
static inline u128 spu_xshw(u128 a) {
    return spu__st(vreinterpretq_u32_s32(vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(spu__ld(a)), 16), 16)));
}
static inline u128 spu_clz(u128 a) {
    return spu__st(vclzq_u32(spu__ld(a)));
}
static inline u128 spu_xswd(u128 a) {
    const uint32x4_t x = spu__ld(a);
    return spu__st(vtrn2q_u32(vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(x), 31)), x));
}
static inline u128 spu_orx(u128 a) {
    /* fold the four words onto word 0 without leaving the vector registers */
    uint32x4_t x = spu__ld(a);
    x = vorrq_u32(x, vextq_u32(x, x, 2));
    x = vorrq_u32(x, vextq_u32(x, x, 1));
    return spu__st(vandq_u32(x, vreinterpretq_u32_u64(vsetq_lane_u64(0xFFFFFFFFull, vdupq_n_u64(0), 0))));
}
static inline u128 spu_gb(u128 a) {
    static const uint32_t w[4] = {8, 4, 2, 1};
    return spu_pref_u32(vaddvq_u32(vmulq_u32(vandq_u32(spu__ld(a), vdupq_n_u32(1)), vld1q_u32(w))));
}
static inline u128 spu_gbh(u128 a) {
    static const uint16_t w[8] = {64, 128, 16, 32, 4, 8, 1, 2};
    return spu_pref_u32(vaddvq_u16(vmulq_u16(vandq_u16(vreinterpretq_u16_u32(spu__ld(a)), vdupq_n_u16(1)), vld1q_u16(w))));
}
static inline u128 spu_gbb(u128 a) {
    /* host bytes 0-7 are SPU bytes 0-7 (result bits 15-8), bytes 8-15 bits 7-0: after three
     * pairwise adds byte 0 holds the high result byte and byte 1 the low one */
    static const uint8_t w[16] = {16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8};
    uint8x16_t m = vandq_u8(vtstq_u8(vreinterpretq_u8_u32(spu__ld(a)), vdupq_n_u8(1)), vld1q_u8(w));
    m = vpaddq_u8(m, m); m = vpaddq_u8(m, m); m = vpaddq_u8(m, m);
    return spu__st(vandq_u32(vreinterpretq_u32_u8(vrev16q_u8(m)), vdupq_n_u32(0xFFFFu) & vreinterpretq_u32_u64(vsetq_lane_u64(0xFFFFFFFFull, vdupq_n_u64(0), 0))));
}
static inline u128 spu_fsm(u128 a) {
    static const uint32_t w[4] = {8, 4, 2, 1};
    return spu__st(vtstq_u32(vdupq_n_u32(a._u32[0]), vld1q_u32(w)));
}
static inline u128 spu_fsmh(u128 a) {
    static const uint16_t w[8] = {64, 128, 16, 32, 4, 8, 1, 2};
    return spu__st(vreinterpretq_u32_u16(vtstq_u16(vdupq_n_u16((uint16_t)a._u32[0]), vld1q_u16(w))));
}
static inline u128 spu_fsmb(u128 a) {
    static const uint8_t w[16] = {16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8};
    const uint8x16_t v = vcombine_u8(vdup_n_u8((uint8_t)(a._u32[0] >> 8)), vdup_n_u8((uint8_t)a._u32[0]));
    return spu__st(vreinterpretq_u32_u8(vtstq_u8(v, vld1q_u8(w))));
}
#else
static inline u128 spu_absdb(u128 a, u128 b) { return spu_absdb_ref(a, b); }
static inline u128 spu_avgb(u128 a, u128 b) { return spu_avgb_ref(a, b); }
static inline u128 spu_ceqb(u128 a, u128 b) { return spu_ceqb_ref(a, b); }
static inline u128 spu_cgtb(u128 a, u128 b) { return spu_cgtb_ref(a, b); }
static inline u128 spu_clgtb(u128 a, u128 b) { return spu_clgtb_ref(a, b); }
static inline u128 spu_ceqh(u128 a, u128 b) { return spu_ceqh_ref(a, b); }
static inline u128 spu_cgth(u128 a, u128 b) { return spu_cgth_ref(a, b); }
static inline u128 spu_clgth(u128 a, u128 b) { return spu_clgth_ref(a, b); }
static inline u128 spu_ah(u128 a, u128 b) { return spu_ah_ref(a, b); }
static inline u128 spu_sfh(u128 a, u128 b) { return spu_sfh_ref(a, b); }
static inline u128 spu_shlh(u128 a, u128 b) { return spu_shlh_ref(a, b); }
static inline u128 spu_roth(u128 a, u128 b) { return spu_roth_ref(a, b); }
static inline u128 spu_rothm(u128 a, u128 b) { return spu_rothm_ref(a, b); }
static inline u128 spu_rothma(u128 a, u128 b) { return spu_rothma_ref(a, b); }
static inline u128 spu_shl(u128 a, u128 b) { return spu_shl_ref(a, b); }
static inline u128 spu_rot(u128 a, u128 b) { return spu_rot_ref(a, b); }
static inline u128 spu_rotm(u128 a, u128 b) { return spu_rotm_ref(a, b); }
static inline u128 spu_rotma(u128 a, u128 b) { return spu_rotma_ref(a, b); }
static inline u128 spu_cg(u128 a, u128 b) { return spu_cg_ref(a, b); }
static inline u128 spu_bg(u128 a, u128 b) { return spu_bg_ref(a, b); }
static inline u128 spu_mpy(u128 a, u128 b) { return spu_mpy_ref(a, b); }
static inline u128 spu_mpyu(u128 a, u128 b) { return spu_mpyu_ref(a, b); }
static inline u128 spu_mpyh(u128 a, u128 b) { return spu_mpyh_ref(a, b); }
static inline u128 spu_mpyhh(u128 a, u128 b) { return spu_mpyhh_ref(a, b); }
static inline u128 spu_mpyhhu(u128 a, u128 b) { return spu_mpyhhu_ref(a, b); }
static inline u128 spu_mpys(u128 a, u128 b) { return spu_mpys_ref(a, b); }
static inline u128 spu_sumb(u128 a, u128 b) { return spu_sumb_ref(a, b); }
static inline u128 spu_ceqbi(u128 a, int32_t imm) { return spu_ceqbi_ref(a, imm); }
static inline u128 spu_cgtbi(u128 a, int32_t imm) { return spu_cgtbi_ref(a, imm); }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { return spu_clgtbi_ref(a, imm); }
static inline u128 spu_ceqhi(u128 a, int32_t imm) { return spu_ceqhi_ref(a, imm); }
static inline u128 spu_cgthi(u128 a, int32_t imm) { return spu_cgthi_ref(a, imm); }
static inline u128 spu_clgthi(u128 a, int32_t imm) { return spu_clgthi_ref(a, imm); }
static inline u128 spu_ahi(u128 a, int32_t imm) { return spu_ahi_ref(a, imm); }
static inline u128 spu_sfhi(u128 a, int32_t imm) { return spu_sfhi_ref(a, imm); }
static inline u128 spu_mpyi(u128 a, int32_t imm) { return spu_mpyi_ref(a, imm); }
static inline u128 spu_mpyui(u128 a, int32_t imm) { return spu_mpyui_ref(a, imm); }
static inline u128 spu_rothi(u128 a, int sh) { return spu_rothi_ref(a, sh); }
static inline u128 spu_roti(u128 a, int sh) { return spu_roti_ref(a, sh); }
static inline u128 spu_rotmahi(u128 a, int i7) { return spu_rotmahi_ref(a, i7); }
static inline u128 spu_rotmai(u128 a, int i7) { return spu_rotmai_ref(a, i7); }
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { return spu_mpya_ref(a, b, c); }
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t) { return spu_mpyhha_ref(a, b, t); }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { return spu_mpyhhau_ref(a, b, t); }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { return spu_addx_ref(a, b, t); }
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { return spu_sfx_ref(a, b, t); }
static inline u128 spu_cgx(u128 a, u128 b, u128 t) { return spu_cgx_ref(a, b, t); }
static inline u128 spu_bgx(u128 a, u128 b, u128 t) { return spu_bgx_ref(a, b, t); }
static inline u128 spu_cntb(u128 a) { return spu_cntb_ref(a); }
static inline u128 spu_xsbh(u128 a) { return spu_xsbh_ref(a); }
static inline u128 spu_xshw(u128 a) { return spu_xshw_ref(a); }
static inline u128 spu_clz(u128 a) { return spu_clz_ref(a); }
static inline u128 spu_xswd(u128 a) { return spu_xswd_ref(a); }
static inline u128 spu_orx(u128 a) { return spu_orx_ref(a); }
static inline u128 spu_gb(u128 a) { return spu_gb_ref(a); }
static inline u128 spu_gbh(u128 a) { return spu_gbh_ref(a); }
static inline u128 spu_gbb(u128 a) { return spu_gbb_ref(a); }
static inline u128 spu_fsm(u128 a) { return spu_fsm_ref(a); }
static inline u128 spu_fsmh(u128 a) { return spu_fsmh_ref(a); }
static inline u128 spu_fsmb(u128 a) { return spu_fsmb_ref(a); }
#endif

#endif /* SPU_HELPERS_H */
