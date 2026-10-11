/* ppu_vmx.h -- exact VMX (AltiVec) semantics for lifted PPU code.
 *
 * Vector registers are held big-endian (byte 0 = the most significant byte,
 * element 0 = the leftmost element), exactly as lvx loads them. Every helper
 * computes into a temporary first, so the destination may alias any source.
 *
 * Semantics follow PowerISA 2.02 VMX as RPCS3's PPU interpreter implements it
 * with its accuracy options on (the oracle of tests/conformance/ppu):
 *   - saturating ops OR the SAT bit (VSCR 0x00000001) into *vscr;
 *   - VSCR[NJ] (0x00010000) flushes denormal float inputs and results to zero;
 *   - a NaN operand propagates (A, then B, then C) with its quiet bit set; a NaN
 *     produced from non-NaN operands is the default QNaN 0x7FC00000.
 * Each helper is the whole instruction, so the lifter emits one call per op.
 */
#ifndef PPU_VMX_H
#define PPU_VMX_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMX_SAT 0x00000001u
#define VMX_NJ  0x00010000u
#define VMX_INL static inline

/* ---- element access (big-endian) ---- */
VMX_INL uint16_t vmx_h(const uint8_t* v, int i) { return (uint16_t)(v[2 * i] << 8 | v[2 * i + 1]); }
VMX_INL uint32_t vmx_w(const uint8_t* v, int i) {
    return (uint32_t)v[4 * i] << 24 | (uint32_t)v[4 * i + 1] << 16 | (uint32_t)v[4 * i + 2] << 8 | v[4 * i + 3];
}
VMX_INL void vmx_sh(uint8_t* v, int i, uint16_t x) { v[2 * i] = (uint8_t)(x >> 8); v[2 * i + 1] = (uint8_t)x; }
VMX_INL void vmx_sw(uint8_t* v, int i, uint32_t x) {
    v[4 * i] = (uint8_t)(x >> 24); v[4 * i + 1] = (uint8_t)(x >> 16); v[4 * i + 2] = (uint8_t)(x >> 8); v[4 * i + 3] = (uint8_t)x;
}
VMX_INL float    vmx_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
VMX_INL uint32_t vmx_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

#define VMX_D  void* d_
#define VMX_A  const void* a_
#define VMX_B  const void* b_
#define VMX_C  const void* c_
#define VMX_ARGS_PREP const uint8_t* a = (const uint8_t*)a_; (void)a; \
    const uint8_t* b = (const uint8_t*)b_; (void)b; uint8_t r[16]
#define VMX_OUT memcpy(d_, r, 16)

/* ---- saturation ---- */
VMX_INL int32_t vmx_sat_s(int64_t x, int64_t lo, int64_t hi, uint32_t* vscr) {
    if (x < lo) { *vscr |= VMX_SAT; return (int32_t)lo; }
    if (x > hi) { *vscr |= VMX_SAT; return (int32_t)hi; }
    return (int32_t)x;
}
VMX_INL uint32_t vmx_sat_u(int64_t x, int64_t hi, uint32_t* vscr) {
    if (x < 0) { *vscr |= VMX_SAT; return 0; }
    if (x > hi) { *vscr |= VMX_SAT; return (uint32_t)hi; }
    return (uint32_t)x;
}

/* ---- float helpers ---- */
VMX_INL uint32_t vmx_flush(uint32_t x, uint32_t vscr) {
    return ((vscr & VMX_NJ) && (x & 0x7F800000u) == 0) ? (x & 0x80000000u) : x;
}
VMX_INL int vmx_isnan(uint32_t x) { return (x & 0x7FFFFFFFu) > 0x7F800000u; }
VMX_INL uint32_t vmx_fix(uint32_t r) { return vmx_isnan(r) ? 0x7FC00000u : r; }
/* NaN precedence: A, B (quieted), else the fixed result */
VMX_INL uint32_t vmx_vnan2(uint32_t r, uint32_t a, uint32_t b) {
    if (vmx_isnan(a)) return a | 0x7FC00000u;
    if (vmx_isnan(b)) return b | 0x7FC00000u;
    return vmx_fix(r);
}
VMX_INL uint32_t vmx_vnan1(uint32_t r, uint32_t b) {
    if (vmx_isnan(b)) return b | 0x7FC00000u;
    return vmx_fix(r);
}

/* ======================================================================
 * integer element-wise
 * ==================================================================== */

#define VMX_BIN(name, cnt, get, set, expr) \
VMX_INL void name(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; \
    for (int i = 0; i < cnt; i++) { uint64_t x = get(a, i), y = get(b, i); (void)x; (void)y; set(r, i, expr); } VMX_OUT; }
VMX_INL uint8_t vmx_b(const uint8_t* v, int i) { return v[i]; }
VMX_INL void vmx_sb(uint8_t* v, int i, uint8_t x) { v[i] = x; }

/* rotates / shifts: count from the low bits of the matching element of B */
VMX_BIN(vmx_vrlb, 16, vmx_b, vmx_sb, (uint8_t)((x << (y & 7)) | (x >> ((8 - (y & 7)) & 7))))
VMX_BIN(vmx_vrlh, 8, vmx_h, vmx_sh, (uint16_t)((x << (y & 15)) | (x >> ((16 - (y & 15)) & 15))))
VMX_BIN(vmx_vrlw, 4, vmx_w, vmx_sw, (uint32_t)((x << (y & 31)) | (x >> ((32 - (y & 31)) & 31))))
VMX_BIN(vmx_vslb, 16, vmx_b, vmx_sb, (uint8_t)(x << (y & 7)))
VMX_BIN(vmx_vslh, 8, vmx_h, vmx_sh, (uint16_t)(x << (y & 15)))
VMX_BIN(vmx_vslw, 4, vmx_w, vmx_sw, (uint32_t)(x << (y & 31)))
VMX_BIN(vmx_vsrb, 16, vmx_b, vmx_sb, (uint8_t)(x >> (y & 7)))
VMX_BIN(vmx_vsrh, 8, vmx_h, vmx_sh, (uint16_t)(x >> (y & 15)))
VMX_BIN(vmx_vsrw, 4, vmx_w, vmx_sw, (uint32_t)(x >> (y & 31)))
VMX_BIN(vmx_vsrab, 16, vmx_b, vmx_sb, (uint8_t)((int8_t)x >> (y & 7)))
VMX_BIN(vmx_vsrah, 8, vmx_h, vmx_sh, (uint16_t)((int16_t)x >> (y & 15)))
VMX_BIN(vmx_vsraw, 4, vmx_w, vmx_sw, (uint32_t)((int32_t)x >> (y & 31)))

/* averages: (a + b + 1) >> 1 without overflow */
VMX_BIN(vmx_vavgub, 16, vmx_b, vmx_sb, (uint8_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavguh, 8, vmx_h, vmx_sh, (uint16_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavguw, 4, vmx_w, vmx_sw, (uint32_t)((x + y + 1) >> 1))
VMX_BIN(vmx_vavgsb, 16, vmx_b, vmx_sb, (uint8_t)(((int64_t)(int8_t)x + (int8_t)y + 1) >> 1))
VMX_BIN(vmx_vavgsh, 8, vmx_h, vmx_sh, (uint16_t)(((int64_t)(int16_t)x + (int16_t)y + 1) >> 1))
VMX_BIN(vmx_vavgsw, 4, vmx_w, vmx_sw, (uint32_t)(((int64_t)(int32_t)x + (int32_t)y + 1) >> 1))

/* carry / borrow out */
VMX_BIN(vmx_vaddcuw, 4, vmx_w, vmx_sw, (uint32_t)((x + y) >> 32))
VMX_BIN(vmx_vsubcuw, 4, vmx_w, vmx_sw, (uint32_t)(x >= y))

/* saturating add / sub */
#define VMX_SATOP(name, cnt, get, set, expr) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP; \
    for (int i = 0; i < cnt; i++) { uint64_t x = get(a, i), y = get(b, i); set(r, i, expr); } VMX_OUT; }
VMX_SATOP(vmx_vaddubs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFF, vscr))
VMX_SATOP(vmx_vadduhs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFFFF, vscr))
VMX_SATOP(vmx_vadduws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_u((int64_t)x + (int64_t)y, 0xFFFFFFFFll, vscr))
VMX_SATOP(vmx_vaddsbs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_s((int64_t)(int8_t)x + (int8_t)y, -128, 127, vscr))
VMX_SATOP(vmx_vaddshs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_s((int64_t)(int16_t)x + (int16_t)y, -32768, 32767, vscr))
VMX_SATOP(vmx_vaddsws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_s((int64_t)(int32_t)x + (int32_t)y, INT32_MIN, INT32_MAX, vscr))
VMX_SATOP(vmx_vsububs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFF, vscr))
VMX_SATOP(vmx_vsubuhs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFFFF, vscr))
VMX_SATOP(vmx_vsubuws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_u((int64_t)x - (int64_t)y, 0xFFFFFFFFll, vscr))
VMX_SATOP(vmx_vsubsbs, 16, vmx_b, vmx_sb, (uint8_t)vmx_sat_s((int64_t)(int8_t)x - (int8_t)y, -128, 127, vscr))
VMX_SATOP(vmx_vsubshs, 8, vmx_h, vmx_sh, (uint16_t)vmx_sat_s((int64_t)(int16_t)x - (int16_t)y, -32768, 32767, vscr))
VMX_SATOP(vmx_vsubsws, 4, vmx_w, vmx_sw, (uint32_t)vmx_sat_s((int64_t)(int32_t)x - (int32_t)y, INT32_MIN, INT32_MAX, vscr))

/* even / odd multiplies: element i of the result is the product of source
 * element 2i (even) or 2i+1 (odd), big-endian numbering */
#define VMX_MUL(name, n, get, set, cast, par) \
VMX_INL void name(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; \
    for (int i = 0; i < n; i++) { int64_t x = (cast)get(a, 2 * i + par), y = (cast)get(b, 2 * i + par); set(r, i, x * y); } VMX_OUT; }
VMX_MUL(vmx_vmuleub, 8, vmx_b, vmx_sh, uint8_t, 0)
VMX_MUL(vmx_vmuloub, 8, vmx_b, vmx_sh, uint8_t, 1)
VMX_MUL(vmx_vmulesb, 8, vmx_b, vmx_sh, int8_t, 0)
VMX_MUL(vmx_vmulosb, 8, vmx_b, vmx_sh, int8_t, 1)
VMX_MUL(vmx_vmuleuh, 4, vmx_h, vmx_sw, uint16_t, 0)
VMX_MUL(vmx_vmulouh, 4, vmx_h, vmx_sw, uint16_t, 1)
VMX_MUL(vmx_vmulesh, 4, vmx_h, vmx_sw, int16_t, 0)
VMX_MUL(vmx_vmulosh, 4, vmx_h, vmx_sw, int16_t, 1)

/* ---- multiply-add / multiply-sum (VA-form) ---- */
VMX_INL void vmx_vmhaddshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) {
        int64_t p = ((int64_t)(int16_t)vmx_h(a, i) * (int16_t)vmx_h(b, i)) >> 15;
        vmx_sh(r, i, (uint16_t)vmx_sat_s(p + (int16_t)vmx_h(c, i), -32768, 32767, vscr));
    } VMX_OUT; }
VMX_INL void vmx_vmhraddshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) {
        int64_t p = ((int64_t)(int16_t)vmx_h(a, i) * (int16_t)vmx_h(b, i) + 0x4000) >> 15;
        vmx_sh(r, i, (uint16_t)vmx_sat_s(p + (int16_t)vmx_h(c, i), -32768, 32767, vscr));
    } VMX_OUT; }
VMX_INL void vmx_vmladduhm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 8; i++) vmx_sh(r, i, (uint16_t)(vmx_h(a, i) * vmx_h(b, i) + vmx_h(c, i))); VMX_OUT; }
VMX_INL void vmx_vmsumubm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) { uint32_t s = vmx_w(c, i);
        for (int j = 0; j < 4; j++) s += (uint32_t)a[4 * i + j] * b[4 * i + j];
        vmx_sw(r, i, s); } VMX_OUT; }
VMX_INL void vmx_vmsummbm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) { uint32_t s = vmx_w(c, i);
        for (int j = 0; j < 4; j++) s += (uint32_t)((int32_t)(int8_t)a[4 * i + j] * (int32_t)b[4 * i + j]);
        vmx_sw(r, i, s); } VMX_OUT; }
VMX_INL void vmx_vmsumuhm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) vmx_sw(r, i, vmx_w(c, i) + (uint32_t)vmx_h(a, 2 * i) * vmx_h(b, 2 * i)
                                                        + (uint32_t)vmx_h(a, 2 * i + 1) * vmx_h(b, 2 * i + 1));
    VMX_OUT; }
VMX_INL void vmx_vmsumuhs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)vmx_w(c, i) + (int64_t)vmx_h(a, 2 * i) * vmx_h(b, 2 * i)
                  + (int64_t)vmx_h(a, 2 * i + 1) * vmx_h(b, 2 * i + 1);
        vmx_sw(r, i, vmx_sat_u(s, 0xFFFFFFFFll, vscr)); } VMX_OUT; }
VMX_INL void vmx_vmsumshm(VMX_D, VMX_A, VMX_B, VMX_C) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) vmx_sw(r, i, (uint32_t)((int32_t)vmx_w(c, i)
        + (int32_t)(int16_t)vmx_h(a, 2 * i) * (int16_t)vmx_h(b, 2 * i)
        + (int32_t)(int16_t)vmx_h(a, 2 * i + 1) * (int16_t)vmx_h(b, 2 * i + 1)));
    VMX_OUT; }
VMX_INL void vmx_vmsumshs(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t* vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(c, i) + (int64_t)(int16_t)vmx_h(a, 2 * i) * (int16_t)vmx_h(b, 2 * i)
                  + (int64_t)(int16_t)vmx_h(a, 2 * i + 1) * (int16_t)vmx_h(b, 2 * i + 1);
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }

/* ---- sums across ---- */
VMX_INL void vmx_vsum4ubs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) { int64_t s = vmx_w(b, i);
        for (int j = 0; j < 4; j++) s += a[4 * i + j];
        vmx_sw(r, i, vmx_sat_u(s, 0xFFFFFFFFll, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum4sbs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) { int64_t s = (int32_t)vmx_w(b, i);
        for (int j = 0; j < 4; j++) s += (int8_t)a[4 * i + j];
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum4shs(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    for (int i = 0; i < 4; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(b, i) + (int16_t)vmx_h(a, 2 * i) + (int16_t)vmx_h(a, 2 * i + 1);
        vmx_sw(r, i, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsum2sws(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    memset(r, 0, 16);
    for (int i = 0; i < 2; i++) {
        int64_t s = (int64_t)(int32_t)vmx_w(a, 2 * i) + (int32_t)vmx_w(a, 2 * i + 1) + (int32_t)vmx_w(b, 2 * i + 1);
        vmx_sw(r, 2 * i + 1, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); } VMX_OUT; }
VMX_INL void vmx_vsumsws(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP;
    memset(r, 0, 16);
    int64_t s = (int32_t)vmx_w(b, 3);
    for (int i = 0; i < 4; i++) s += (int32_t)vmx_w(a, i);
    vmx_sw(r, 3, (uint32_t)vmx_sat_s(s, INT32_MIN, INT32_MAX, vscr)); VMX_OUT; }

/* ---- packs: A's elements fill the high half, B's the low half ---- */
#define VMX_PACK(name, n, get, set, conv) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t* vscr) { VMX_ARGS_PREP; (void)vscr; \
    for (int i = 0; i < n; i++) { int64_t x = get(a, i); set(r, i, conv); } \
    for (int i = 0; i < n; i++) { int64_t x = get(b, i); set(r, n + i, conv); } VMX_OUT; }
VMX_PACK(vmx_vpkuhum, 8, vmx_h, vmx_sb, (uint8_t)x)
VMX_PACK(vmx_vpkuwum, 4, vmx_w, vmx_sh, (uint16_t)x)
VMX_PACK(vmx_vpkuhus, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_u(x, 0xFF, vscr))
VMX_PACK(vmx_vpkuwus, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_u(x, 0xFFFF, vscr))
VMX_PACK(vmx_vpkshus, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_u((int16_t)x, 0xFF, vscr))
VMX_PACK(vmx_vpkswus, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_u((int32_t)x, 0xFFFF, vscr))
VMX_PACK(vmx_vpkshss, 8, vmx_h, vmx_sb, (uint8_t)vmx_sat_s((int16_t)x, -128, 127, vscr))
VMX_PACK(vmx_vpkswss, 4, vmx_w, vmx_sh, (uint16_t)vmx_sat_s((int32_t)x, -32768, 32767, vscr))
VMX_PACK(vmx_vpkpx, 4, vmx_w, vmx_sh, (uint16_t)(((x >> 24) & 1) << 15 | ((x >> 19) & 0x1F) << 10 |
                                                 ((x >> 11) & 0x1F) << 5 | ((x >> 3) & 0x1F)))

/* ---- unpacks (sign-extend; pixel 1:5:5:5 -> 8:8:8:8) ---- */
VMX_INL void vmx_vupkpx(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) { uint16_t h = vmx_h(b, i + 4 * lo);
        r[4 * i] = (h & 0x8000) ? 0xFF : 0; r[4 * i + 1] = (h >> 10) & 0x1F;
        r[4 * i + 2] = (h >> 5) & 0x1F; r[4 * i + 3] = h & 0x1F; } VMX_OUT; }
VMX_INL void vmx_vupksb(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 8; i++) vmx_sh(r, i, (uint16_t)(int16_t)(int8_t)b[i + 8 * lo]); VMX_OUT; }
VMX_INL void vmx_vupksh(VMX_D, VMX_B, int lo) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) vmx_sw(r, i, (uint32_t)(int32_t)(int16_t)vmx_h(b, i + 4 * lo)); VMX_OUT; }

/* ---- whole-vector shifts ---- */
/* vsl / vsr: RPCS3 shifts each byte by that byte's own count (B byte & 7),
 * funnelling bits in from the neighbour; with all counts equal (the ISA's
 * defined case) this is the 128-bit shift. */
VMX_INL void vmx_vsl(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP;
    for (int i = 0; i < 16; i++) { int s = b[i] & 7; uint8_t nx = i < 15 ? a[i + 1] : 0;
        r[i] = (uint8_t)((a[i] << s) | (s ? nx >> (8 - s) : 0)); } VMX_OUT; }
VMX_INL void vmx_vsr(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP;
    for (int i = 0; i < 16; i++) { int s = b[i] & 7; uint8_t pv = i > 0 ? a[i - 1] : 0;
        r[i] = (uint8_t)((a[i] >> s) | (s ? pv << (8 - s) : 0)); } VMX_OUT; }
/* vslo / vsro: by octets, count from bits 1:4 of B's last byte */
VMX_INL void vmx_vslo(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int n = (b[15] >> 3) & 15;
    for (int i = 0; i < 16; i++) r[i] = i + n < 16 ? a[i + n] : 0; VMX_OUT; }
VMX_INL void vmx_vsro(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int n = (b[15] >> 3) & 15;
    for (int i = 0; i < 16; i++) r[i] = i - n >= 0 ? a[i - n] : 0; VMX_OUT; }

/* ======================================================================
 * floating point
 * ==================================================================== */

#define VMX_FBIN(name, op, flush_in) \
VMX_INL void name(VMX_D, VMX_A, VMX_B, uint32_t vscr) { VMX_ARGS_PREP; \
    for (int i = 0; i < 4; i++) { uint32_t x = vmx_w(a, i), y = vmx_w(b, i); \
        if (flush_in) { x = vmx_flush(x, vscr); y = vmx_flush(y, vscr); } \
        uint32_t z = vmx_u(op); vmx_sw(r, i, vmx_flush(vmx_vnan2(z, x, y), vscr)); } VMX_OUT; }

VMX_INL float vmx_maxf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == 0 && y == 0) return signbit(x) ? y : x;      /* max(-0,+0) = +0 */
    return x > y ? x : y;
}
VMX_INL float vmx_minf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == 0 && y == 0) return signbit(x) ? x : y;      /* min(-0,+0) = -0 */
    return x < y ? x : y;
}
VMX_FBIN(vmx_vaddfp, vmx_f(x) + vmx_f(y), 1)
VMX_FBIN(vmx_vsubfp, vmx_f(x) - vmx_f(y), 1)
VMX_FBIN(vmx_vmaxfp, vmx_maxf(vmx_f(x), vmx_f(y)), 0)
VMX_FBIN(vmx_vminfp, vmx_minf(vmx_f(x), vmx_f(y)), 0)

VMX_INL void vmx_vmaddfp(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        float x = vmx_f(vmx_flush(vmx_w(a, i), vscr)), y = vmx_f(vmx_flush(vmx_w(b, i), vscr)),
              z = vmx_f(vmx_flush(vmx_w(c, i), vscr));
        vmx_sw(r, i, vmx_flush(vmx_fix(vmx_u(fmaf(x, z, y))), vscr)); } VMX_OUT; }
VMX_INL void vmx_vnmsubfp(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) { VMX_ARGS_PREP; const uint8_t* c = (const uint8_t*)c_;
    for (int i = 0; i < 4; i++) {
        float x = vmx_f(vmx_flush(vmx_w(a, i), vscr)), y = vmx_f(vmx_flush(vmx_w(b, i), vscr)),
              z = vmx_f(vmx_flush(vmx_w(c, i), vscr));
        vmx_sw(r, i, vmx_flush(vmx_fix(vmx_u(-fmaf(x, z, -y))), vscr)); } VMX_OUT; }

#define VMX_FUN(name, expr, flush_in, nanmode) \
VMX_INL void name(VMX_D, VMX_B, uint32_t vscr) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16]; \
    for (int i = 0; i < 4; i++) { uint32_t y = vmx_w(b, i); if (flush_in) y = vmx_flush(y, vscr); \
        float f = vmx_f(y); uint32_t z = vmx_u(expr); \
        vmx_sw(r, i, nanmode == 2 ? vmx_flush(vmx_vnan1(z, y), vscr) : vmx_fix(z)); } VMX_OUT; }
VMX_FUN(vmx_vrefp, 1.0f / f, 1, 2)
VMX_FUN(vmx_vrsqrtefp, 1.0f / sqrtf(f), 1, 2)
VMX_FUN(vmx_vexptefp, exp2f(f), 0, 1)
VMX_FUN(vmx_vlogefp, log2f(f), 0, 1)
VMX_FUN(vmx_vrfin, nearbyintf(f), 0, 2)      /* host default rounding: to nearest even */
VMX_FUN(vmx_vrfiz, truncf(f), 0, 2)
VMX_FUN(vmx_vrfip, ceilf(f), 1, 2)
VMX_FUN(vmx_vrfim, floorf(f), 1, 2)

/* int <-> float with a 2^uimm scale */
VMX_INL void vmx_vcfux(VMX_D, VMX_B, unsigned uimm) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) vmx_sw(r, i, vmx_u(ldexpf((float)vmx_w(b, i), -(int)uimm))); VMX_OUT; }
VMX_INL void vmx_vcfsx(VMX_D, VMX_B, unsigned uimm) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) vmx_sw(r, i, vmx_u(ldexpf((float)(int32_t)vmx_w(b, i), -(int)uimm))); VMX_OUT; }
VMX_INL void vmx_vctsxs(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) { float f = vmx_f(vmx_w(b, i)) * ldexpf(1.0f, (int)uimm); int32_t v;
        if (f != f) v = 0;
        else if (f < -2147483648.0f) { v = INT32_MIN; *vscr |= VMX_SAT; }
        else if (f >= 2147483648.0f) { v = INT32_MAX; *vscr |= VMX_SAT; }
        else v = (int32_t)f;
        vmx_sw(r, i, (uint32_t)v); } VMX_OUT; }
VMX_INL void vmx_vctuxs(VMX_D, VMX_B, unsigned uimm, uint32_t* vscr) { const uint8_t* b = (const uint8_t*)b_; uint8_t r[16];
    for (int i = 0; i < 4; i++) { float f = vmx_f(vmx_w(b, i)) * ldexpf(1.0f, (int)uimm); uint32_t v;
        if (f != f) v = 0;
        else if (f < 0.0f) { v = 0; *vscr |= VMX_SAT; }
        else if (f >= 4294967296.0f) { v = 0xFFFFFFFFu; *vscr |= VMX_SAT; }
        else v = (uint32_t)f;
        vmx_sw(r, i, v); } VMX_OUT; }

/* ---- compares; record forms return the CR6 nibble [all, 0, none, 0] ---- */
#define VMX_CMP(name, cnt, get, set, test) \
VMX_INL uint32_t name(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int all = 1, none = 1; \
    for (int i = 0; i < cnt; i++) { uint64_t x = get(a, i), y = get(b, i); (void)x; (void)y; int t = (test); \
        all &= t; none &= !t; set(r, i, t ? ~0ull : 0); } VMX_OUT; return (uint32_t)(all << 3 | none << 1); }
VMX_INL void vmx_sb64(uint8_t* v, int i, uint64_t x) { v[i] = (uint8_t)x; }
VMX_INL void vmx_sh64(uint8_t* v, int i, uint64_t x) { vmx_sh(v, i, (uint16_t)x); }
VMX_INL void vmx_sw64(uint8_t* v, int i, uint64_t x) { vmx_sw(v, i, (uint32_t)x); }
VMX_CMP(vmx_vcmpequb, 16, vmx_b, vmx_sb64, x == y)
VMX_CMP(vmx_vcmpequh, 8, vmx_h, vmx_sh64, x == y)
VMX_CMP(vmx_vcmpequw, 4, vmx_w, vmx_sw64, x == y)
VMX_CMP(vmx_vcmpgtub, 16, vmx_b, vmx_sb64, x > y)
VMX_CMP(vmx_vcmpgtuh, 8, vmx_h, vmx_sh64, x > y)
VMX_CMP(vmx_vcmpgtuw, 4, vmx_w, vmx_sw64, x > y)
VMX_CMP(vmx_vcmpgtsb, 16, vmx_b, vmx_sb64, (int8_t)x > (int8_t)y)
VMX_CMP(vmx_vcmpgtsh, 8, vmx_h, vmx_sh64, (int16_t)x > (int16_t)y)
VMX_CMP(vmx_vcmpgtsw, 4, vmx_w, vmx_sw64, (int32_t)x > (int32_t)y)
VMX_CMP(vmx_vcmpeqfp, 4, vmx_w, vmx_sw64, vmx_f((uint32_t)x) == vmx_f((uint32_t)y))
VMX_CMP(vmx_vcmpgefp, 4, vmx_w, vmx_sw64, vmx_f((uint32_t)x) >= vmx_f((uint32_t)y))
VMX_CMP(vmx_vcmpgtfp, 4, vmx_w, vmx_sw64, vmx_f((uint32_t)x) > vmx_f((uint32_t)y))
/* vcmpbfp: bit 31 = !(a <= b), bit 30 = !(a >= -b); CR6 EQ = every lane in bounds */
VMX_INL uint32_t vmx_vcmpbfp(VMX_D, VMX_A, VMX_B) { VMX_ARGS_PREP; int inb = 1;
    for (int i = 0; i < 4; i++) { float x = vmx_f(vmx_w(a, i)), y = vmx_f(vmx_w(b, i));
        uint32_t v = (!(x <= y) ? 0x80000000u : 0) | (!(x >= -y) ? 0x40000000u : 0);
        inb &= v == 0; vmx_sw(r, i, v); } VMX_OUT; return (uint32_t)(inb << 1); }

#ifdef __cplusplus
}
#endif
#endif /* PPU_VMX_H */
