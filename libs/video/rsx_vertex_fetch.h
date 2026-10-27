/*
 * ps3recomp - RSX guest vertex fetch
 *
 * Reading a vertex out of guest memory is RSX semantics, not host-API
 * semantics: big-endian components, the NV4097 type encodings, the vertex
 * frequency divisor, and the context-DMA bit in the array offset. Nothing
 * about it is specific to D3D12, Metal or anything else.
 *
 * It nevertheless existed twice -- once in rsx_d3d12_backend.c and again in
 * rsx_metal_backend.m, whose header openly said "ported from the D3D12
 * backend's read_vp_vertex" -- and the two copies had already drifted apart in
 * two ways that matter:
 *
 *   - A DISABLED attribute array must feed the constant vertex attribute
 *     register (NV4097_SET_VERTEX_DATA4F_M), exactly like glColor4f with the
 *     colour array switched off. The D3D12 copy learned this the hard way
 *     (defaulting to black multiplied Rubber Ducky's duck texture away); the
 *     Metal copy substituted caller-supplied literals and never read the
 *     register at all.
 *
 *   - A LOCAL (VRAM) array offset must resolve as local. Routing it through
 *     cellGcmResolveOffset lets the IO table win for any offset whose page the
 *     IO window also covers, and the array silently reads from empty main
 *     memory. The D3D12 copy resolves LOCAL explicitly; the Metal copy did
 *     not, which is that bug still sitting there.
 *
 * So this is the D3D12 version, which is the one that has met real games,
 * lifted out to a single definition. A third backend that needs vertices calls
 * this rather than porting the logic a third time.
 */
#ifndef PS3RECOMP_RSX_VERTEX_FETCH_H
#define PS3RECOMP_RSX_VERTEX_FETCH_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest memory, and the RSX offset -> effective address resolvers.
 *
 * Declared here because every backend that reads guest data needs them, and
 * each one was otherwise carrying its own local `extern` block -- which is how
 * the Metal backend ended up with a use whose declaration lived inside a
 * duplicated fetch routine, and lost it when that duplicate was removed.
 *
 * location: 0 = MAIN (IO-mapped system memory), 1 = LOCAL (VRAM). Resolving
 * LOCAL through cellGcmResolveOffset instead lets the IO table shadow it; see
 * the note on rsx_fetch_attrib below. */
extern u8* vm_base;
u32 cellGcmResolveOffset(u32 offset);
u32 cellGcmResolveLocated(int location, u32 offset);

/* Big-endian scalar reads. Every component in a guest vertex array is stored
 * in the PS3's byte order, so these are the bottom of every fetch path. */
float rsx_rd_bef(const u8* p);      /* 4-byte IEEE float          */
float rsx_rd_half_be(const u8* p);  /* 2-byte IEEE half -> float  */

/* Fetch attribute `idx` (0..15) of vertex `vi` as a float4 into `out`.
 *
 * `out` is always fully written: when the attribute's array is disabled or
 * has a zero stride, it receives the constant vertex attribute register for
 * that slot, which is what the hardware feeds. Components the attribute does
 * not supply (size < 4) keep that register's value.
 *
 * Handles the frequency divisor (instancing) and resolves the array offset
 * through the correct context DMA. Reads guest memory; requires vm_base.
 */
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4]);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VERTEX_FETCH_H */
