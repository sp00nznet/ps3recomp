/*
 * ps3recomp - compact CPU vertex-fetch planning for the live RSX renderer
 *
 * The legacy renderer expands every guest attribute into a 16 x float4
 * vertex.  This helper preserves the same conversion rules while allowing a
 * caller to fetch only the attributes statically read by the active vertex
 * program.  It is D3D12-independent so format/bounds behavior can be tested
 * without creating a device.
 */
#ifndef PS3RECOMP_RSX_VERTEX_COMPACT_H
#define PS3RECOMP_RSX_VERTEX_COMPACT_H

#include "rsx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const u8* (*rsx_vertex_guest_ptr_fn)(
    void* user, u32 location, u32 offset, u32 min_bytes);

typedef struct rsx_vertex_layout_plan {
    u32 mask;
    u32 stride;
    u8 count;
    u8 attrs[RSX_DSP_NUM_VERTEX_ATTR];
    s8 slot_by_attr[RSX_DSP_NUM_VERTEX_ATTR];
} rsx_vertex_layout_plan;

typedef struct rsx_vertex_ref {
    u32 vertex_id;
    u32 base_index;
} rsx_vertex_ref;

typedef struct rsx_vertex_remap_slot {
    u64 key;
    u32 value;
    u32 generation;
} rsx_vertex_remap_slot;

typedef struct rsx_vertex_remap {
    u32* occurrence_to_unique;
    u32 occurrence_capacity;
    rsx_vertex_remap_slot* slots;
    u32 slot_capacity;
    u32 generation;
    /* Direct table for a draw whose references share one base_index and
     * span a small vertex-id range: id - lo indexes these, tagged by
     * direct_generation like the hash slots. */
    u32* direct_tag;
    u32* direct_value;
    u32 direct_capacity;
    u32 direct_generation;
} rsx_vertex_remap;

typedef struct rsx_vertex_fetch_attr {
    rsx_dsp_vertex_attr desc;
    u32 elem_size;
    u32 stride;
    float default_value[4];
    const u8* stable_base;
    u32 stable_first;
    u32 stable_last;
} rsx_vertex_fetch_attr;

typedef struct rsx_vertex_fetch_plan {
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_attr attr[RSX_DSP_NUM_VERTEX_ATTR];
    u32 base_offset;
    u32 divider_mask;
    rsx_vertex_guest_ptr_fn guest_ptr;
    void* guest_user;
    /* INLINE_ARRAY source: when set, the vertices came down the FIFO and the
     * array offsets/locations mean nothing -- every attribute is read out of
     * this stream instead. See rsx_vertex_inline_layout. */
    const u8* inline_data;
    u32 inline_bytes;
    u32 inline_off[RSX_DSP_NUM_VERTEX_ATTR]; /* byte offset within a vertex */
    u32 inline_stride;                       /* decoded once, at _init      */
} rsx_vertex_fetch_plan;

/* Deterministic ascending-register layout; each selected register remains a
 * float4 and retains its original ATTR semantic index. */
void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask);

/* Byte offset of each attribute within one INLINE_ARRAY vertex, and the
 * vertex size as the return value.
 *
 * An inline stream carries no per-array offset registers -- the only thing
 * that says where an attribute sits is the VTXFMT declaration, and the
 * hardware rule is that the ENABLED attributes pack in ascending register
 * order. The declared stride wins over the packed size when the two differ
 * (a format may pad); the packed size is the fallback when no enabled array
 * declares a stride. Shared so the hoisted, legacy and reference fetch paths
 * cannot drift on it. */
u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR]);

/* Point a plan at an INLINE_ARRAY stream instead of guest memory. Call
 * between _init and _prepare; a NULL `data` leaves the plan reading guest
 * memory, so an ordinary draw can call it unconditionally and pay nothing. */
void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes);

/* Decode all 16 descriptors/defaults once for a draw. */
void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user);

/* Attempt one bounds-checked contiguous mapping per used attribute.  Any
 * attribute whose complete span is not contiguous safely falls back to the
 * legacy per-element resolver in rsx_vertex_fetch_one. */
void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count);

/* Fetch one compact vertex in layout order.  Disabled attributes use their
 * RSX current/default value.  Enabled non-position attributes retain the
 * legacy fallback-to-default behavior on an unsupported/invalid fetch; an
 * invalid enabled ATTR0 remains fatal. */
int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out);

/* rsx_vertex_fetch_one for refs[0..count), vertex i at out + i * stride,
 * filled attribute by attribute with decoders chosen once per attribute.
 * Same output bit for bit, and the same verdict: 0 exactly when some
 * reference's fetch_one would return 0 (the output is then incomplete). */
int rsx_vertex_fetch_all(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out);

/* Collapse repeated (vertex_id, base_index) references in first-use order.
 * refs is rewritten in place with the unique prefix.  The occurrence map
 * remains valid until the next call using the same remap object. */
int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count);

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence);

/* The draw's whole index range instead of its unique vertices: when every
 * reference shares one base_index and the range lo..hi of vertex ids is no
 * longer than the reference count, refs becomes lo, lo+1, ..., hi (*out_count
 * = hi - lo + 1) and occurrence i maps to vertex_id - lo. Returns 0, leaving
 * refs untouched, when that does not apply (use rsx_vertex_remap_build).
 *
 * A mesh's indices cover its vertex range almost exactly (inFamous: span 1.00x
 * the unique count, every draw), so this fetches the same vertices without
 * deduplicating, in address order. Occurrence i always satisfies
 * refs[rsx_vertex_remap_index(remap, i)] == the original refs[i]. */
int rsx_vertex_remap_build_range(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* out_count);

/* The hash-table form of rsx_vertex_remap_build, always. The ordinary call
 * takes a direct table when the references allow it; this is the reference
 * that path is tested against. */
int rsx_vertex_remap_build_hashed(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count);

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap);

/* Select the live renderer's triangle topology path.  A remapped triangle
 * list must use an index buffer to reconstruct duplicate occurrences, just
 * like converted strips, fans, and quads.  Returns zero for unsupported
 * primitive classes. */
int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed);

/* Public for focused format/endian tests. */
int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4]);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_VERTEX_COMPACT_H */
