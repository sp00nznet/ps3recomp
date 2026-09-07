/*
 * ps3recomp - PPU memory access for recompiled code
 *
 * All PS3 memory is big-endian.  These functions translate a 32-bit PS3
 * virtual address to a host pointer via vm_base, then perform a load or
 * store with the appropriate byte swap.
 *
 * Atomic operations (lwarx/stwcx) are emulated with C11 atomics.
 */

#ifndef PPU_MEMORY_H
#define PPU_MEMORY_H

#include "../../include/ps3emu/endian.h"
#include "ppu_context.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Base pointer -- set by the VM manager at startup.
 *
 * Host pointer to the beginning of the PS3 32-bit address space mapping.
 * All guest address translation is:  host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Address translation
 * -----------------------------------------------------------------------*/
/* Translate a guest pointer PARAMETER to a host pointer, preserving NULL.
 *
 * HLE entry points receive pointer arguments as raw 32-bit GUEST addresses
 * (ppu_hle.cpp passes the PPC register through untouched), so dereferencing
 * one directly hits whatever host address happens to share that number. That
 * is the most common bug in libs/ -- cellFsRead read an entire sound bank to
 * an untranslated address this way. Fifteen files had each pasted their own
 * copy of this macro; this is the one definition.
 *
 * NOTE this gives you a host pointer to BIG-ENDIAN memory. It is right for
 * byte buffers and for structs of u8, but a u32/u64/float field still needs
 * the vm_read and vm_write accessors below (or an explicit swap) -- the cast
 * alone does not fix endianness.
 */
#ifndef GUEST_PTR
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#endif

static inline void* vm_translate(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint8_t* vm_ptr8(uint32_t addr)  { return (uint8_t*)vm_translate(addr); }
static inline uint16_t* vm_ptr16(uint32_t addr) { return (uint16_t*)vm_translate(addr); }
static inline uint32_t* vm_ptr32(uint32_t addr) { return (uint32_t*)vm_translate(addr); }
static inline uint64_t* vm_ptr64(uint32_t addr) { return (uint64_t*)vm_translate(addr); }

/* ---------------------------------------------------------------------------
 * Loads -- read from big-endian guest memory, return host-endian value.
 * -----------------------------------------------------------------------*/
static inline uint8_t vm_read8(uint32_t addr)
{
    return *vm_ptr8(addr);
}

static inline uint16_t vm_read16(uint32_t addr)
{
    uint16_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap16(raw);
}

static inline uint32_t vm_read32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap32(raw);
}

static inline uint64_t vm_read64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap64(raw);
}

static inline float vm_read_f32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap32(raw);
    float result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

static inline double vm_read_f64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap64(raw);
    double result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

/* ---------------------------------------------------------------------------
 * Stores -- write host-endian value into big-endian guest memory.
 * -----------------------------------------------------------------------*/
/* PPU reservation: a plain store to a reserved word must break other threads'
 * reservations (real-PPC granule semantics). g_resv_store_active gates it so the
 * default path is one predictable branch. Defined in ppu_loader.cpp. */
#ifdef __cplusplus
extern "C" {
#endif
extern int  g_resv_store_active;
void        ppu_resv_break_store(uint64_t ea);

/* PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c). A PPU store to
 * a 128-byte line an SPU has reserved with GETLLAR has to serialize through the
 * SPU lock-line lock -- otherwise it can land inside a PUTLLC's compare and be
 * written straight back over -- and has to raise SPU_EVENT_LR, which is how an
 * SPURS idle service parked in `rdch SPU_RdEventStat` learns work was posted.
 * Declared here rather than pulling spu_coherency.h into every HLE module.
 *
 * spu_coh_is_reserved is a load of a global that stays zero until an SPU
 * reserves its first line, so the ordinary store pays one predictable branch. */
int         spu_coh_is_reserved(uint32_t addr);
void        spu_lockline_lock(void);
void        spu_lockline_unlock(void);
void        spu_coh_notify_write(uint32_t addr);
#ifdef __cplusplus
}
#endif

#define VM_WRITE_COH(addr, src, n)                                            \
    do {                                                                      \
        if (spu_coh_is_reserved((uint32_t)(addr))) {                          \
            spu_lockline_lock();                                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
            spu_coh_notify_write((uint32_t)(addr));                           \
            spu_lockline_unlock();                                            \
        } else {                                                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
        }                                                                     \
    } while (0)

/* ---------------------------------------------------------------------------
 * Write-watch hook for the INLINE writers.
 *
 * LBP_WW (ppu_loader.cpp) logs stores to a watched address along with the guest
 * function responsible. Recompiled guest code calls the extern vm_write* family
 * there, so it is covered -- but everything in libs/ uses the inline family
 * below, and those stores were invisible to it. That makes "nothing writes this
 * address" a claim the watch cannot actually support, which is exactly the sort
 * of thing worth not being wrong about: an HLE writing a zero looks identical
 * to nobody writing at all.
 *
 * Cost is two compares against a pair that stay 0 unless LBP_WW is set, on the
 * HLE path only -- guest stores never reach here.
 * -----------------------------------------------------------------------*/
extern uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width);

#define PS3_WW_CHECK(a, v, w)                                                 \
    do {                                                                      \
        if ((a) >= g_ww_lo && (a) < g_ww_hi)                                  \
            ps3_ww_report_inline((uint32_t)(a), (uint64_t)(v), (w));          \
    } while (0)

static inline void vm_write8(uint32_t addr, uint8_t val)
{
    PS3_WW_CHECK(addr, val, 1);
    VM_WRITE_COH(addr, &val, 1);
}

static inline void vm_write16(uint32_t addr, uint16_t val)
{
    PS3_WW_CHECK(addr, val, 2);
    uint16_t raw = ps3_bswap16(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write32(uint32_t addr, uint32_t val)
{
    PS3_WW_CHECK(addr, val, 4);
    uint32_t raw = ps3_bswap32(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
    if (g_resv_store_active > 0) ppu_resv_break_store(addr);
}

static inline void vm_write64(uint32_t addr, uint64_t val)
{
    PS3_WW_CHECK(addr, val, 8);
    uint64_t raw = ps3_bswap64(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
    if (g_resv_store_active > 0) ppu_resv_break_store(addr);
}

static inline void vm_write_f32(uint32_t addr, float val)
{
    uint32_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write32(addr, tmp);
}

static inline void vm_write_f64(uint32_t addr, double val)
{
    uint64_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write64(addr, tmp);
}

/* ---------------------------------------------------------------------------
 * Block copies (for string / bulk data movement)
 * -----------------------------------------------------------------------*/
static inline void vm_memcpy_from(void* host_dst, uint32_t guest_src, size_t len)
{
    memcpy(host_dst, vm_ptr8(guest_src), len);
}

/* A block store is a store to every 128-byte line it touches, so it breaks
 * an SPU reservation on any of them the same way vm_write32 does on one:
 * under the lock-line lock, with the lost-reservation event raised per line.
 * Only the lines that are reserved are notified, and a block that touches
 * none takes the plain path. */
static inline int vm_block_reserved(uint32_t guest_dst, size_t len)
{
    if (len == 0) return 0;
    uint32_t first = guest_dst & ~127u;
    uint32_t last  = (uint32_t)(guest_dst + (len - 1)) & ~127u;
    for (uint32_t line = first; ; line += 128u) {
        if (spu_coh_is_reserved(line)) return 1;
        if (line == last) break;
    }
    return 0;
}

static inline void vm_block_notify(uint32_t guest_dst, size_t len)
{
    uint32_t first = guest_dst & ~127u;
    uint32_t last  = (uint32_t)(guest_dst + (len - 1)) & ~127u;
    for (uint32_t line = first; ; line += 128u) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
}

static inline void vm_memcpy_to(uint32_t guest_dst, const void* host_src, size_t len)
{
    if (vm_block_reserved(guest_dst, len)) {
        spu_lockline_lock();
        memcpy(vm_ptr8(guest_dst), host_src, len);
        vm_block_notify(guest_dst, len);
        spu_lockline_unlock();
    } else {
        memcpy(vm_ptr8(guest_dst), host_src, len);
    }
}

static inline void vm_memset(uint32_t guest_dst, int val, size_t len)
{
    if (vm_block_reserved(guest_dst, len)) {
        spu_lockline_lock();
        memset(vm_ptr8(guest_dst), val, len);
        vm_block_notify(guest_dst, len);
        spu_lockline_unlock();
    } else {
        memset(vm_ptr8(guest_dst), val, len);
    }
}

/* ---------------------------------------------------------------------------
 * Atomic operations -- lwarx / stwcx emulation
 *
 * PowerPC reservation-based atomics:
 *   lwarx  rD, rA, rB   -- Load word and reserve (sets reservation)
 *   stwcx. rS, rA, rB   -- Store word conditional (clears reservation)
 *                           Sets CR0 EQ bit on success.
 *
 * We emulate this using C11 compare-and-swap on the host.
 * -----------------------------------------------------------------------*/

static inline uint32_t ppu_lwarx(ppu_context* ctx, uint32_t addr)
{
    /* Read the current value from memory (big-endian, so swap). */
    uint32_t raw;
    _Atomic(uint32_t)* atom = (_Atomic(uint32_t)*)vm_ptr32(addr);
    raw = atomic_load_explicit(atom, memory_order_acquire);

    ctx->reserve_addr  = addr;
    ctx->reserve_value = raw;
    ctx->reserve_valid = 1;

    return ps3_bswap32(raw);
}

static inline int ppu_stwcx(ppu_context* ctx, uint32_t addr, uint32_t val)
{
    if (!ctx->reserve_valid || ctx->reserve_addr != addr) {
        /* No reservation or address mismatch -- fail */
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));
        ctx->reserve_valid = 0;
        return 0;
    }

    uint32_t expected = (uint32_t)ctx->reserve_value;
    uint32_t desired  = ps3_bswap32(val);
    _Atomic(uint32_t)* atom = (_Atomic(uint32_t)*)vm_ptr32(addr);

    /* On a line an SPU has reserved, commit under the lock-line lock and notify
     * -- the same serialization the coherent stores above use, so a PPU commit
     * can never land inside an SPU PUTLLC's compare/commit window (nor the
     * reverse), and the reserving SPU gets its lost-reservation event. */
    int ok;
    if (spu_coh_is_reserved(addr)) {
        spu_lockline_lock();
        ok = atomic_compare_exchange_strong_explicit(
            atom, &expected, desired,
            memory_order_acq_rel, memory_order_acquire);
        if (ok) spu_coh_notify_write(addr);
        spu_lockline_unlock();
    } else {
        ok = atomic_compare_exchange_strong_explicit(
            atom, &expected, desired,
            memory_order_acq_rel, memory_order_acquire);
    }

    ctx->reserve_valid = 0;

    if (ok)
        ppu_cr_set(ctx, 0, PPU_CR_EQ | (PPU_CR_SO * ppu_xer_get_so(ctx)));
    else
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));

    return ok;
}

/* 64-bit variants: ldarx / stdcx. */
static inline uint64_t ppu_ldarx(ppu_context* ctx, uint32_t addr)
{
    _Atomic(uint64_t)* atom = (_Atomic(uint64_t)*)vm_ptr64(addr);
    uint64_t raw = atomic_load_explicit(atom, memory_order_acquire);

    ctx->reserve_addr  = addr;
    ctx->reserve_value = raw;
    ctx->reserve_valid = 1;

    return ps3_bswap64(raw);
}

static inline int ppu_stdcx(ppu_context* ctx, uint32_t addr, uint64_t val)
{
    if (!ctx->reserve_valid || ctx->reserve_addr != addr) {
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));
        ctx->reserve_valid = 0;
        return 0;
    }

    uint64_t expected = ctx->reserve_value;
    uint64_t desired  = ps3_bswap64(val);
    _Atomic(uint64_t)* atom = (_Atomic(uint64_t)*)vm_ptr64(addr);

    int ok;
    if (spu_coh_is_reserved(addr)) {
        spu_lockline_lock();
        ok = atomic_compare_exchange_strong_explicit(
            atom, &expected, desired,
            memory_order_acq_rel, memory_order_acquire);
        if (ok) spu_coh_notify_write(addr);
        spu_lockline_unlock();
    } else {
        ok = atomic_compare_exchange_strong_explicit(
            atom, &expected, desired,
            memory_order_acq_rel, memory_order_acquire);
    }

    ctx->reserve_valid = 0;

    if (ok)
        ppu_cr_set(ctx, 0, PPU_CR_EQ | (PPU_CR_SO * ppu_xer_get_so(ctx)));
    else
        ppu_cr_set(ctx, 0, PPU_CR_SO * ppu_xer_get_so(ctx));

    return ok;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_MEMORY_H */
