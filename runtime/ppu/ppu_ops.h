/*
 * ps3recomp - PPU operation macros for recompiled code
 *
 * These macros translate PowerPC 64-bit (PPU) instructions into C operations
 * on a ppu_context*.  The recompiler emits calls to these macros in place of
 * the original instructions.
 *
 * Naming convention:
 *   PPU_<mnemonic>(ctx, ...)
 *
 * All macros take `ctx` as the first argument (pointer to ppu_context).
 */

#ifndef PPU_OPS_H
#define PPU_OPS_H

#include "ppu_context.h"
#include "ppu_memory.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Integer Arithmetic
 * =====================================================================*/

/* 64-bit add / sub */
#define PPU_ADD(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (ctx)->gpr[ra] + (ctx)->gpr[rb]

#define PPU_ADDI(ctx, rd, ra, simm) \
    (ctx)->gpr[rd] = ((ra) ? (ctx)->gpr[ra] : 0) + (int64_t)(int16_t)(simm)

#define PPU_ADDIS(ctx, rd, ra, simm) \
    (ctx)->gpr[rd] = ((ra) ? (ctx)->gpr[ra] : 0) + ((int64_t)(int16_t)(simm) << 16)

#define PPU_ADDIC(ctx, rd, ra, simm) do {                           \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (uint64_t)(int64_t)(int16_t)(simm);               \
    uint64_t _r = _a + _b;                                          \
    ppu_xer_set_ca(ctx, _r < _a);                                   \
    (ctx)->gpr[rd] = _r;                                            \
} while(0)

#define PPU_ADDC(ctx, rd, ra, rb) do {                              \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _r = _a + (ctx)->gpr[rb];                              \
    ppu_xer_set_ca(ctx, _r < _a);                                   \
    (ctx)->gpr[rd] = _r;                                            \
} while(0)

#define PPU_ADDE(ctx, rd, ra, rb) do {                              \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (ctx)->gpr[rb];                                   \
    uint64_t _ca = ppu_xer_get_ca(ctx) ? 1ULL : 0ULL;              \
    uint64_t _r = _a + _b + _ca;                                    \
    ppu_xer_set_ca(ctx, (_r < _a) || (_ca && _r == _a));            \
    (ctx)->gpr[rd] = _r;                                            \
} while(0)

#define PPU_ADDZE(ctx, rd, ra) do {                                 \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _ca = ppu_xer_get_ca(ctx) ? 1ULL : 0ULL;              \
    uint64_t _r = _a + _ca;                                         \
    ppu_xer_set_ca(ctx, _r < _a);                                   \
    (ctx)->gpr[rd] = _r;                                            \
} while(0)

#define PPU_SUBF(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (ctx)->gpr[rb] - (ctx)->gpr[ra]

#define PPU_SUBFC(ctx, rd, ra, rb) do {                             \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (ctx)->gpr[rb];                                   \
    (ctx)->gpr[rd] = _b - _a;                                       \
    ppu_xer_set_ca(ctx, _a <= _b);                                  \
} while(0)

#define PPU_SUBFIC(ctx, rd, ra, simm) do {                          \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (uint64_t)(int64_t)(int16_t)(simm);               \
    (ctx)->gpr[rd] = _b - _a;                                       \
    ppu_xer_set_ca(ctx, _a <= _b);                                  \
} while(0)

#define PPU_SUBFE(ctx, rd, ra, rb) do {                             \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (ctx)->gpr[rb];                                   \
    uint64_t _ca = ppu_xer_get_ca(ctx) ? 1ULL : 0ULL;              \
    uint64_t _r = ~_a + _b + _ca;                                   \
    ppu_xer_set_ca(ctx, (_ca ? (_a <= _b) : (_a < _b)));            \
    (ctx)->gpr[rd] = _r;                                            \
} while(0)

#define PPU_NEG(ctx, rd, ra) \
    (ctx)->gpr[rd] = (uint64_t)(-(int64_t)(ctx)->gpr[ra])

/* 32-bit multiply / divide (PPC32 carry-over, results truncated to 32 bits) */
#define PPU_MULLW(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)((int32_t)(ctx)->gpr[ra] * (int32_t)(ctx)->gpr[rb])

#define PPU_MULHW(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int32_t) \
        (((int64_t)(int32_t)(ctx)->gpr[ra] * (int64_t)(int32_t)(ctx)->gpr[rb]) >> 32)

#define PPU_MULHWU(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)(uint32_t) \
        (((uint64_t)(uint32_t)(ctx)->gpr[ra] * (uint64_t)(uint32_t)(ctx)->gpr[rb]) >> 32)

#define PPU_MULLD(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)((int64_t)(ctx)->gpr[ra] * (int64_t)(ctx)->gpr[rb])

#define PPU_DIVW(ctx, rd, ra, rb) do {                              \
    int32_t _a = (int32_t)(ctx)->gpr[ra];                           \
    int32_t _b = (int32_t)(ctx)->gpr[rb];                           \
    (ctx)->gpr[rd] = (_b != 0 && !(_a == INT32_MIN && _b == -1))    \
        ? (uint64_t)(int64_t)(_a / _b) : 0;                        \
} while(0)

#define PPU_DIVWU(ctx, rd, ra, rb) do {                             \
    uint32_t _a = (uint32_t)(ctx)->gpr[ra];                         \
    uint32_t _b = (uint32_t)(ctx)->gpr[rb];                         \
    (ctx)->gpr[rd] = _b ? (uint64_t)(_a / _b) : 0;                 \
} while(0)

#define PPU_DIVD(ctx, rd, ra, rb) do {                              \
    int64_t _a = (int64_t)(ctx)->gpr[ra];                           \
    int64_t _b = (int64_t)(ctx)->gpr[rb];                           \
    (ctx)->gpr[rd] = (_b != 0 && !(_a == INT64_MIN && _b == -1))    \
        ? (uint64_t)(_a / _b) : 0;                                  \
} while(0)

#define PPU_DIVDU(ctx, rd, ra, rb) do {                             \
    uint64_t _a = (ctx)->gpr[ra];                                   \
    uint64_t _b = (ctx)->gpr[rb];                                   \
    (ctx)->gpr[rd] = _b ? _a / _b : 0;                             \
} while(0)

#define PPU_MULLI(ctx, rd, ra, simm) \
    (ctx)->gpr[rd] = (uint64_t)((int64_t)(ctx)->gpr[ra] * (int64_t)(int16_t)(simm))

/* sign/zero extensions */
#define PPU_EXTSB(ctx, rd, rs) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int8_t)(ctx)->gpr[rs]

#define PPU_EXTSH(ctx, rd, rs) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int16_t)(ctx)->gpr[rs]

#define PPU_EXTSW(ctx, rd, rs) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int32_t)(ctx)->gpr[rs]

/* count leading zeros */
#define PPU_CNTLZW(ctx, rd, rs) do {                                \
    uint32_t _v = (uint32_t)(ctx)->gpr[rs];                         \
    (ctx)->gpr[rd] = _v ? (uint64_t)__builtin_clz(_v) : 32;        \
} while(0)

#define PPU_CNTLZD(ctx, rd, rs) do {                                \
    uint64_t _v = (ctx)->gpr[rs];                                   \
    (ctx)->gpr[rd] = _v ? (uint64_t)__builtin_clzll(_v) : 64;      \
} while(0)

/* =========================================================================
 * Logical
 * =====================================================================*/

#define PPU_AND(ctx, rd, rs, rb)    (ctx)->gpr[rd] = (ctx)->gpr[rs] & (ctx)->gpr[rb]
#define PPU_ANDI(ctx, rd, rs, imm)  (ctx)->gpr[rd] = (ctx)->gpr[rs] & (uint64_t)(uint16_t)(imm)
#define PPU_ANDIS(ctx, rd, rs, imm) (ctx)->gpr[rd] = (ctx)->gpr[rs] & ((uint64_t)(uint16_t)(imm) << 16)
#define PPU_OR(ctx, rd, rs, rb)     (ctx)->gpr[rd] = (ctx)->gpr[rs] | (ctx)->gpr[rb]
#define PPU_ORI(ctx, rd, rs, imm)   (ctx)->gpr[rd] = (ctx)->gpr[rs] | (uint64_t)(uint16_t)(imm)
#define PPU_ORIS(ctx, rd, rs, imm)  (ctx)->gpr[rd] = (ctx)->gpr[rs] | ((uint64_t)(uint16_t)(imm) << 16)
#define PPU_XOR(ctx, rd, rs, rb)    (ctx)->gpr[rd] = (ctx)->gpr[rs] ^ (ctx)->gpr[rb]
#define PPU_XORI(ctx, rd, rs, imm)  (ctx)->gpr[rd] = (ctx)->gpr[rs] ^ (uint64_t)(uint16_t)(imm)
#define PPU_XORIS(ctx, rd, rs, imm) (ctx)->gpr[rd] = (ctx)->gpr[rs] ^ ((uint64_t)(uint16_t)(imm) << 16)
#define PPU_NAND(ctx, rd, rs, rb)   (ctx)->gpr[rd] = ~((ctx)->gpr[rs] & (ctx)->gpr[rb])
#define PPU_NOR(ctx, rd, rs, rb)    (ctx)->gpr[rd] = ~((ctx)->gpr[rs] | (ctx)->gpr[rb])
#define PPU_EQV(ctx, rd, rs, rb)    (ctx)->gpr[rd] = ~((ctx)->gpr[rs] ^ (ctx)->gpr[rb])
#define PPU_ANDC(ctx, rd, rs, rb)   (ctx)->gpr[rd] = (ctx)->gpr[rs] & ~(ctx)->gpr[rb]
#define PPU_ORC(ctx, rd, rs, rb)    (ctx)->gpr[rd] = (ctx)->gpr[rs] | ~(ctx)->gpr[rb]

/* =========================================================================
 * Shift / Rotate
 * =====================================================================*/

/* 32-bit shifts (result is sign-extended to 64 bits for some) */
#define PPU_SLW(ctx, rd, rs, rb) \
    (ctx)->gpr[rd] = ((ctx)->gpr[rb] & 0x3F) < 32 \
        ? (uint64_t)(uint32_t)((uint32_t)(ctx)->gpr[rs] << ((ctx)->gpr[rb] & 0x1F)) : 0

#define PPU_SRW(ctx, rd, rs, rb) \
    (ctx)->gpr[rd] = ((ctx)->gpr[rb] & 0x3F) < 32 \
        ? (uint64_t)((uint32_t)(ctx)->gpr[rs] >> ((ctx)->gpr[rb] & 0x1F)) : 0

#define PPU_SRAW(ctx, rd, rs, rb) do {                              \
    int32_t  _s = (int32_t)(ctx)->gpr[rs];                          \
    uint32_t _n = (uint32_t)(ctx)->gpr[rb] & 0x3F;                  \
    if (_n < 32) {                                                   \
        int32_t _r = _s >> _n;                                       \
        ppu_xer_set_ca(ctx, (_s < 0) && (_r << _n) != _s);          \
        (ctx)->gpr[rd] = (uint64_t)(int64_t)_r;                     \
    } else {                                                         \
        ppu_xer_set_ca(ctx, _s < 0);                                \
        (ctx)->gpr[rd] = (uint64_t)(int64_t)(_s >> 31);             \
    }                                                                \
} while(0)

#define PPU_SRAWI(ctx, rd, rs, sh) do {                             \
    int32_t  _s = (int32_t)(ctx)->gpr[rs];                          \
    int32_t  _r = _s >> (sh);                                        \
    ppu_xer_set_ca(ctx, (_s < 0) && (_r << (sh)) != _s);            \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)_r;                         \
} while(0)

/* 64-bit shifts */
#define PPU_SLD(ctx, rd, rs, rb) \
    (ctx)->gpr[rd] = ((ctx)->gpr[rb] & 0x7F) < 64 \
        ? (ctx)->gpr[rs] << ((ctx)->gpr[rb] & 0x3F) : 0

#define PPU_SRD(ctx, rd, rs, rb) \
    (ctx)->gpr[rd] = ((ctx)->gpr[rb] & 0x7F) < 64 \
        ? (ctx)->gpr[rs] >> ((ctx)->gpr[rb] & 0x3F) : 0

#define PPU_SRAD(ctx, rd, rs, rb) do {                              \
    int64_t  _s = (int64_t)(ctx)->gpr[rs];                          \
    uint32_t _n = (uint32_t)(ctx)->gpr[rb] & 0x7F;                  \
    if (_n < 64) {                                                   \
        int64_t _r = _s >> _n;                                       \
        ppu_xer_set_ca(ctx, (_s < 0) && (_r << _n) != _s);          \
        (ctx)->gpr[rd] = (uint64_t)_r;                               \
    } else {                                                         \
        ppu_xer_set_ca(ctx, _s < 0);                                \
        (ctx)->gpr[rd] = (uint64_t)(_s >> 63);                      \
    }                                                                \
} while(0)

#define PPU_SRADI(ctx, rd, rs, sh) do {                             \
    int64_t _s = (int64_t)(ctx)->gpr[rs];                           \
    int64_t _r = _s >> (sh);                                         \
    ppu_xer_set_ca(ctx, (_s < 0) && (_r << (sh)) != _s);            \
    (ctx)->gpr[rd] = (uint64_t)_r;                                   \
} while(0)

/* Rotate left word immediate then AND with mask */
#define PPU_RLWINM(ctx, rd, rs, sh, mb, me) do {                   \
    uint32_t _v = (uint32_t)(ctx)->gpr[rs];                         \
    uint32_t _r = (_v << (sh)) | (_v >> (32 - (sh)));               \
    uint32_t _mask;                                                  \
    if ((mb) <= (me))                                                \
        _mask = (0xFFFFFFFFu >> (mb)) & (0xFFFFFFFFu << (31 - (me))); \
    else                                                             \
        _mask = (0xFFFFFFFFu >> (mb)) | (0xFFFFFFFFu << (31 - (me))); \
    (ctx)->gpr[rd] = (uint64_t)(_r & _mask);                        \
} while(0)

/* Rotate left word immediate then mask insert */
#define PPU_RLWIMI(ctx, rd, rs, sh, mb, me) do {                   \
    uint32_t _v = (uint32_t)(ctx)->gpr[rs];                         \
    uint32_t _r = (_v << (sh)) | (_v >> (32 - (sh)));               \
    uint32_t _mask;                                                  \
    if ((mb) <= (me))                                                \
        _mask = (0xFFFFFFFFu >> (mb)) & (0xFFFFFFFFu << (31 - (me))); \
    else                                                             \
        _mask = (0xFFFFFFFFu >> (mb)) | (0xFFFFFFFFu << (31 - (me))); \
    (ctx)->gpr[rd] = (uint64_t)((_r & _mask) | ((uint32_t)(ctx)->gpr[rd] & ~_mask)); \
} while(0)

/* Rotate left word then AND with mask (register shift) */
#define PPU_RLWNM(ctx, rd, rs, rb, mb, me) do {                   \
    uint32_t _v = (uint32_t)(ctx)->gpr[rs];                         \
    uint32_t _sh = (uint32_t)(ctx)->gpr[rb] & 0x1F;                 \
    uint32_t _r = (_v << _sh) | (_v >> (32 - _sh));                  \
    uint32_t _mask;                                                  \
    if ((mb) <= (me))                                                \
        _mask = (0xFFFFFFFFu >> (mb)) & (0xFFFFFFFFu << (31 - (me))); \
    else                                                             \
        _mask = (0xFFFFFFFFu >> (mb)) | (0xFFFFFFFFu << (31 - (me))); \
    (ctx)->gpr[rd] = (uint64_t)(_r & _mask);                        \
} while(0)

/* Rotate left doubleword immediate then clear left */
#define PPU_RLDICL(ctx, rd, rs, sh, mb) do {                       \
    uint64_t _v = (ctx)->gpr[rs];                                   \
    uint64_t _r = (_v << (sh)) | (_v >> (64 - (sh)));               \
    uint64_t _mask = (mb) ? (0xFFFFFFFFFFFFFFFFull >> (mb)) : 0xFFFFFFFFFFFFFFFFull; \
    (ctx)->gpr[rd] = _r & _mask;                                    \
} while(0)

/* Rotate left doubleword immediate then clear right */
#define PPU_RLDICR(ctx, rd, rs, sh, me) do {                       \
    uint64_t _v = (ctx)->gpr[rs];                                   \
    uint64_t _r = (_v << (sh)) | (_v >> (64 - (sh)));               \
    uint64_t _mask = 0xFFFFFFFFFFFFFFFFull << (63 - (me));           \
    (ctx)->gpr[rd] = _r & _mask;                                    \
} while(0)

/* =========================================================================
 * Comparison (sets CR fields)
 * =====================================================================*/

#define PPU_CMPW(ctx, bf, ra, rb) \
    ppu_cr_set_s32(ctx, bf, (int32_t)(ctx)->gpr[ra], (int32_t)(ctx)->gpr[rb])

#define PPU_CMPLW(ctx, bf, ra, rb) \
    ppu_cr_set_u32(ctx, bf, (uint32_t)(ctx)->gpr[ra], (uint32_t)(ctx)->gpr[rb])

#define PPU_CMPWI(ctx, bf, ra, simm) \
    ppu_cr_set_s32(ctx, bf, (int32_t)(ctx)->gpr[ra], (int32_t)(int16_t)(simm))

#define PPU_CMPLWI(ctx, bf, ra, uimm) \
    ppu_cr_set_u32(ctx, bf, (uint32_t)(ctx)->gpr[ra], (uint32_t)(uint16_t)(uimm))

#define PPU_CMPD(ctx, bf, ra, rb) \
    ppu_cr_set_s64(ctx, bf, (int64_t)(ctx)->gpr[ra], (int64_t)(ctx)->gpr[rb])

#define PPU_CMPLD(ctx, bf, ra, rb) \
    ppu_cr_set_u64(ctx, bf, (ctx)->gpr[ra], (ctx)->gpr[rb])

#define PPU_CMPDI(ctx, bf, ra, simm) \
    ppu_cr_set_s64(ctx, bf, (int64_t)(ctx)->gpr[ra], (int64_t)(int16_t)(simm))

#define PPU_CMPLDI(ctx, bf, ra, uimm) \
    ppu_cr_set_u64(ctx, bf, (ctx)->gpr[ra], (uint64_t)(uint16_t)(uimm))

/* Record forms: set CR0 from result */
#define PPU_RC_S32(ctx, rd) \
    ppu_cr_set_s32(ctx, 0, (int32_t)(ctx)->gpr[rd], 0)

#define PPU_RC_S64(ctx, rd) \
    ppu_cr_set_s64(ctx, 0, (int64_t)(ctx)->gpr[rd], 0)

/* =========================================================================
 * Load / Store (big-endian memory)
 * =====================================================================*/

#define PPU_LBZ(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = (uint64_t)vm_read8((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LHZ(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = (uint64_t)vm_read16((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LHA(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int16_t)vm_read16((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LWZ(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = (uint64_t)vm_read32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LWA(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = (uint64_t)(int64_t)(int32_t)vm_read32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LD(ctx, rd, ra, offset) \
    (ctx)->gpr[rd] = vm_read64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LFS(ctx, fd, ra, offset) \
    (ctx)->fpr[fd] = (double)vm_read_f32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

#define PPU_LFD(ctx, fd, ra, offset) \
    (ctx)->fpr[fd] = vm_read_f64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)))

/* Update forms: EA is written back to rA */
#define PPU_LBZU(ctx, rd, ra, offset) do {                          \
    uint32_t _ea = (uint32_t)((ctx)->gpr[ra] + (int16_t)(offset));  \
    (ctx)->gpr[rd] = (uint64_t)vm_read8(_ea);                       \
    (ctx)->gpr[ra] = _ea;                                            \
} while(0)

#define PPU_LWZU(ctx, rd, ra, offset) do {                          \
    uint32_t _ea = (uint32_t)((ctx)->gpr[ra] + (int16_t)(offset));  \
    (ctx)->gpr[rd] = (uint64_t)vm_read32(_ea);                      \
    (ctx)->gpr[ra] = _ea;                                            \
} while(0)

/* Stores */
#define PPU_STB(ctx, rs, ra, offset) \
    vm_write8((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (uint8_t)(ctx)->gpr[rs])

#define PPU_STH(ctx, rs, ra, offset) \
    vm_write16((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (uint16_t)(ctx)->gpr[rs])

#define PPU_STW(ctx, rs, ra, offset) \
    vm_write32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (uint32_t)(ctx)->gpr[rs])

#define PPU_STD(ctx, rs, ra, offset) \
    vm_write64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (ctx)->gpr[rs])

#define PPU_STFS(ctx, fs, ra, offset) \
    vm_write_f32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (float)(ctx)->fpr[fs])

#define PPU_STFD(ctx, fs, ra, offset) \
    vm_write_f64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (int16_t)(offset)), (ctx)->fpr[fs])

/* Update store forms */
#define PPU_STWU(ctx, rs, ra, offset) do {                          \
    uint32_t _ea = (uint32_t)((ctx)->gpr[ra] + (int16_t)(offset));  \
    vm_write32(_ea, (uint32_t)(ctx)->gpr[rs]);                       \
    (ctx)->gpr[ra] = _ea;                                            \
} while(0)

#define PPU_STDU(ctx, rs, ra, offset) do {                          \
    uint32_t _ea = (uint32_t)((ctx)->gpr[ra] + (int16_t)(offset));  \
    vm_write64(_ea, (ctx)->gpr[rs]);                                 \
    (ctx)->gpr[ra] = _ea;                                            \
} while(0)

/* Indexed forms */
#define PPU_LBZX(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)vm_read8((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]))

#define PPU_LHZX(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)vm_read16((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]))

#define PPU_LWZX(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = (uint64_t)vm_read32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]))

#define PPU_LDX(ctx, rd, ra, rb) \
    (ctx)->gpr[rd] = vm_read64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]))

#define PPU_STBX(ctx, rs, ra, rb) \
    vm_write8((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]), (uint8_t)(ctx)->gpr[rs])

#define PPU_STHX(ctx, rs, ra, rb) \
    vm_write16((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]), (uint16_t)(ctx)->gpr[rs])

#define PPU_STWX(ctx, rs, ra, rb) \
    vm_write32((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]), (uint32_t)(ctx)->gpr[rs])

#define PPU_STDX(ctx, rs, ra, rb) \
    vm_write64((uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]), (ctx)->gpr[rs])

/* =========================================================================
 * Branch helpers
 * =====================================================================*/

/* Save return address in LR, jump to target (used by recompiler for bl) */
#define PPU_CALL_FUNC(ctx, target_func, return_addr) do {           \
    (ctx)->lr = (return_addr);                                       \
    target_func(ctx);                                                \
} while(0)

/* Indirect call via function pointer table */
typedef void (*ppu_recompiled_fn)(ppu_context*);

#define PPU_LOOKUP_FUNC(table, addr) \
    ((ppu_recompiled_fn)(table)[(addr) >> 2])

/* Branch condition testing */
#define PPU_CR_BIT(ctx, bit) \
    (((ctx)->cr >> (31 - (bit))) & 1)

/* LR / CTR management */
#define PPU_MTLR(ctx, rs)  (ctx)->lr  = (ctx)->gpr[rs]
#define PPU_MFLR(ctx, rd)  (ctx)->gpr[rd] = (ctx)->lr
#define PPU_MTCTR(ctx, rs) (ctx)->ctr = (ctx)->gpr[rs]
#define PPU_MFCTR(ctx, rd) (ctx)->gpr[rd] = (ctx)->ctr

/* SPR access */
#define PPU_MFXER(ctx, rd) (ctx)->gpr[rd] = (uint64_t)(ctx)->xer
#define PPU_MTXER(ctx, rs) (ctx)->xer = (uint32_t)(ctx)->gpr[rs]

/* =========================================================================
 * VMX / AltiVec vector operations (selected subset)
 *
 * Vector register lanes are indexed in big-endian order:
 *   element 0 = most significant bytes.
 * =====================================================================*/

/* Load/store vector from/to memory */
#define PPU_LVX(ctx, vd, ra, rb) do {                               \
    uint32_t _ea = (uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]); \
    _ea &= ~0xFu;  /* 16-byte aligned */                            \
    u128* _dst = &(ctx)->vr[vd];                                    \
    _dst->_u64[0] = vm_read64(_ea);                                  \
    _dst->_u64[1] = vm_read64(_ea + 8);                              \
} while(0)

#define PPU_STVX(ctx, vs, ra, rb) do {                              \
    uint32_t _ea = (uint32_t)(((ra) ? (ctx)->gpr[ra] : 0) + (ctx)->gpr[rb]); \
    _ea &= ~0xFu;                                                   \
    const u128* _src = &(ctx)->vr[vs];                              \
    vm_write64(_ea,     _src->_u64[0]);                              \
    vm_write64(_ea + 8, _src->_u64[1]);                              \
} while(0)

/* Vector add word */
#define PPU_VADDUWM(ctx, vd, va, vb) do {                          \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._u32[_i] = (ctx)->vr[va]._u32[_i] + (ctx)->vr[vb]._u32[_i]; \
} while(0)

/* Vector subtract word */
#define PPU_VSUBUWM(ctx, vd, va, vb) do {                          \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._u32[_i] = (ctx)->vr[va]._u32[_i] - (ctx)->vr[vb]._u32[_i]; \
} while(0)

/* Vector AND / OR / XOR */
#define PPU_VAND(ctx, vd, va, vb) do {                             \
    (ctx)->vr[vd]._u64[0] = (ctx)->vr[va]._u64[0] & (ctx)->vr[vb]._u64[0]; \
    (ctx)->vr[vd]._u64[1] = (ctx)->vr[va]._u64[1] & (ctx)->vr[vb]._u64[1]; \
} while(0)

#define PPU_VOR(ctx, vd, va, vb) do {                              \
    (ctx)->vr[vd]._u64[0] = (ctx)->vr[va]._u64[0] | (ctx)->vr[vb]._u64[0]; \
    (ctx)->vr[vd]._u64[1] = (ctx)->vr[va]._u64[1] | (ctx)->vr[vb]._u64[1]; \
} while(0)

#define PPU_VXOR(ctx, vd, va, vb) do {                             \
    (ctx)->vr[vd]._u64[0] = (ctx)->vr[va]._u64[0] ^ (ctx)->vr[vb]._u64[0]; \
    (ctx)->vr[vd]._u64[1] = (ctx)->vr[va]._u64[1] ^ (ctx)->vr[vb]._u64[1]; \
} while(0)

/* Vector splat immediate signed word */
#define PPU_VSPLTISW(ctx, vd, simm) do {                           \
    int32_t _val = (int32_t)(int8_t)((simm) & 0x1F | ((simm) & 0x10 ? 0xE0 : 0)); \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._s32[_i] = _val;                              \
} while(0)

/* Vector splat word */
#define PPU_VSPLTW(ctx, vd, vb, uimm) do {                        \
    uint32_t _val = (ctx)->vr[vb]._u32[(uimm) & 3];                \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._u32[_i] = _val;                              \
} while(0)

/* Vector float add / sub / mul */
#define PPU_VADDFP(ctx, vd, va, vb) do {                           \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._f32[_i] = (ctx)->vr[va]._f32[_i] + (ctx)->vr[vb]._f32[_i]; \
} while(0)

#define PPU_VSUBFP(ctx, vd, va, vb) do {                           \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._f32[_i] = (ctx)->vr[va]._f32[_i] - (ctx)->vr[vb]._f32[_i]; \
} while(0)

#define PPU_VMADDFP(ctx, vd, va, vc, vb) do {                     \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._f32[_i] = (ctx)->vr[va]._f32[_i] * (ctx)->vr[vc]._f32[_i] + (ctx)->vr[vb]._f32[_i]; \
} while(0)

/* Vector permute */
#define PPU_VPERM(ctx, vd, va, vb, vc) do {                       \
    u128 _tmp;                                                      \
    uint8_t _ab[32];                                                \
    memcpy(_ab, &(ctx)->vr[va], 16);                                \
    memcpy(_ab + 16, &(ctx)->vr[vb], 16);                           \
    for (int _i = 0; _i < 16; _i++)                                \
        _tmp._u8[_i] = _ab[(ctx)->vr[vc]._u8[_i] & 0x1F];          \
    (ctx)->vr[vd] = _tmp;                                           \
} while(0)

/* Vector compare equal word */
#define PPU_VCMPEQUW(ctx, vd, va, vb) do {                         \
    for (int _i = 0; _i < 4; _i++)                                 \
        (ctx)->vr[vd]._u32[_i] = ((ctx)->vr[va]._u32[_i] == (ctx)->vr[vb]._u32[_i]) \
            ? 0xFFFFFFFFu : 0;                                      \
} while(0)

/* Vector merge high/low word */
#define PPU_VMRGHW(ctx, vd, va, vb) do {                           \
    u128 _tmp;                                                      \
    _tmp._u32[0] = (ctx)->vr[va]._u32[0];                          \
    _tmp._u32[1] = (ctx)->vr[vb]._u32[0];                          \
    _tmp._u32[2] = (ctx)->vr[va]._u32[1];                          \
    _tmp._u32[3] = (ctx)->vr[vb]._u32[1];                          \
    (ctx)->vr[vd] = _tmp;                                           \
} while(0)

#define PPU_VMRGLW(ctx, vd, va, vb) do {                           \
    u128 _tmp;                                                      \
    _tmp._u32[0] = (ctx)->vr[va]._u32[2];                          \
    _tmp._u32[1] = (ctx)->vr[vb]._u32[2];                          \
    _tmp._u32[2] = (ctx)->vr[va]._u32[3];                          \
    _tmp._u32[3] = (ctx)->vr[vb]._u32[3];                          \
    (ctx)->vr[vd] = _tmp;                                           \
} while(0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_OPS_H */
