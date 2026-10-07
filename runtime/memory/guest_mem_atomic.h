/*
 * ps3recomp - guest memory accesses with the Cell's single-copy atomicity.
 *
 * On Cell an aligned load or store of up to 8 bytes is single-copy atomic:
 * another PPU thread or an SPU's DMA sees all of it or none of it. Guest code
 * depends on that -- a spin flag read with a plain lwz while another thread
 * stores it, a lock-free fast path reading a word a peer updates -- and does
 * not wrap those accesses in anything. Emulated with memcpy they are C data
 * races: undefined behaviour the compiler may tear, merge or cache, and what
 * ThreadSanitizer reports on every SPURS suite.
 *
 * These helpers do aligned 1/2/4/8-byte guest accesses as relaxed atomics
 * (the same single instruction on x86-64 and ARM64, so no cost) and fall back
 * to memcpy only when the address is misaligned, where the guest access was
 * not atomic either. Bulk copies (DMA, a 128-byte reservation line) move whole
 * aligned doublewords atomically, which is at least as strong as the MFC.
 * Values stay in guest (big-endian) byte order; callers byte-swap.
 */
#ifndef PS3RECOMP_GUEST_MEM_ATOMIC_H
#define PS3RECOMP_GUEST_MEM_ATOMIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GM_RELAXED __ATOMIC_RELAXED

static inline uint8_t gm_load8(const uint8_t* p)
{
    return __atomic_load_n(p, GM_RELAXED);
}
static inline uint16_t gm_load16(const uint8_t* p)
{
    if (((uintptr_t)p & 1u) == 0) return __atomic_load_n((const uint16_t*)p, GM_RELAXED);
    uint16_t v; memcpy(&v, p, 2); return v;
}
static inline uint32_t gm_load32(const uint8_t* p)
{
    if (((uintptr_t)p & 3u) == 0) return __atomic_load_n((const uint32_t*)p, GM_RELAXED);
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t gm_load64(const uint8_t* p)
{
    if (((uintptr_t)p & 7u) == 0) return __atomic_load_n((const uint64_t*)p, GM_RELAXED);
    uint64_t v; memcpy(&v, p, 8); return v;
}

static inline void gm_store8(uint8_t* p, uint8_t v)
{
    __atomic_store_n(p, v, GM_RELAXED);
}
static inline void gm_store16(uint8_t* p, uint16_t v)
{
    if (((uintptr_t)p & 1u) == 0) { __atomic_store_n((uint16_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 2);
}
static inline void gm_store32(uint8_t* p, uint32_t v)
{
    if (((uintptr_t)p & 3u) == 0) { __atomic_store_n((uint32_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 4);
}
static inline void gm_store64(uint8_t* p, uint64_t v)
{
    if (((uintptr_t)p & 7u) == 0) { __atomic_store_n((uint64_t*)p, v, GM_RELAXED); return; }
    memcpy(p, &v, 8);
}

/* guest -> host */
static inline void gm_copy_from(void* dst, const uint8_t* g, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    size_t i = 0;
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) d[i] = gm_load8(g + i);
    for (; i + 8 <= n; i += 8) { uint64_t v = gm_load64(g + i); memcpy(d + i, &v, 8); }
    for (; i < n; i++) d[i] = gm_load8(g + i);
}

/* host -> guest */
static inline void gm_copy_to(uint8_t* g, const void* src, size_t n)
{
    const uint8_t* s = (const uint8_t*)src;
    size_t i = 0;
    for (; i < n && (((uintptr_t)(g + i)) & 7u); i++) gm_store8(g + i, s[i]);
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, s + i, 8); gm_store64(g + i, v); }
    for (; i < n; i++) gm_store8(g + i, s[i]);
}

/* A guest store of n bytes from a host buffer: one atomic access for the
 * natural sizes, a word-wise copy otherwise. */
static inline void gm_store_bytes(uint8_t* g, const void* src, size_t n)
{
    switch (n) {
    case 1: gm_store8(g, *(const uint8_t*)src); return;
    case 2: { uint16_t v; memcpy(&v, src, 2); gm_store16(g, v); return; }
    case 4: { uint32_t v; memcpy(&v, src, 4); gm_store32(g, v); return; }
    case 8: { uint64_t v; memcpy(&v, src, 8); gm_store64(g, v); return; }
    default: gm_copy_to(g, src, n); return;
    }
}

/* Compare n guest bytes against a host snapshot (a reservation line). */
static inline int gm_equal(const uint8_t* g, const void* snap, size_t n)
{
    uint8_t buf[256];
    while (n) {
        size_t k = n < sizeof buf ? n : sizeof buf;
        gm_copy_from(buf, g, k);
        if (memcmp(buf, snap, k)) return 0;
        g += k; snap = (const uint8_t*)snap + k; n -= k;
    }
    return 1;
}

#endif
