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

/* Renderer default for a disabled attribute. The legacy white ATTR3 fallback
 * applies only before a current color has been explicitly written. Shared by
 * the compact and legacy fetch paths. */
void rsx_vertex_fetch_default(const rsx_dispatch* rsx, u32 attr, float out[4]);

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

/* rsx_vertex_fetch_one for refs[0..count) into consecutive compact vertices
 * at out (layout.stride apart), with the same values and the same failure:
 * 0 when some vertex's enabled ATTR0 cannot be fetched. Walks attribute by
 * attribute so each vertex type decodes in a loop of its own. */
int rsx_vertex_fetch_many(
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

/* ---- converted-vertex cache ----------------------------------------------
 *
 * A draw engine converts a draw's vertices to compact float4 vertices and
 * rebuilds its topology into an index buffer, on the CPU, every time. A game
 * repeats most of its draws unchanged: in Resistance 3's Haven firefight 95-97%
 * of ~7800 draws a frame had the same indices, layout and vertex bytes as a draw
 * of this frame or the last, and their conversion was a third of the RSX
 * thread. This cache keeps the converted vertices and indices of a draw in
 * memory the GPU reads, keyed by a hash of everything they were made from:
 * index bytes, layout, attribute state and the vertex bytes themselves. A
 * changed byte is a different key, so nothing is ever invalidated; entries
 * nobody uses age out.
 *
 * The cache only does bookkeeping. Memory comes from the backend as chunks
 * (a GPU buffer and a CPU pointer to its contents); blocks are handed out
 * from them by a buddy allocator. A block the GPU may still read is not
 * reused: the engine tags every use with the serial of the submission it
 * goes into and passes in the serial the GPU has completed. */

typedef struct rsx_vc_key {
    u64 lo, hi;
} rsx_vc_key;

/* 128-bit key of n bytes, chained on seed. Not cryptographic, and a 64-bit
 * margin: two different inputs share a key with a probability of about
 * 2^-64. */
rsx_vc_key rsx_vc_hash(const void* data, size_t n, rsx_vc_key seed);
rsx_vc_key rsx_vc_mix(rsx_vc_key k, u64 v);

typedef struct rsx_vc_chunks {
    void* user;
    /* A chunk of `bytes` the GPU can read; *cpu gets its contents. Returns a
     * handle > 0, or 0 when it cannot. */
    u32 (*create)(void* user, u32 bytes, u8** cpu);
    void (*release)(void* user, u32 chunk);
} rsx_vc_chunks;

typedef struct rsx_vc_entry {
    rsx_vc_key key;
    u32 chunk;          /* backend chunk handle                              */
    u32 offset;         /* of the block in the chunk; vertices start here    */
    u32 ib_off;         /* indices, from the start of the chunk; 0 = none    */
    u32 n_verts;        /* compact vertices stored                           */
    u32 n_draw;         /* indices to draw, or vertices when not indexed     */
    u32 stride;
    u32 topology;       /* the engine's own topology value                   */
    u8  indexed;
    u8  order;          /* buddy order of the block                          */
    u64 last_serial;    /* submission that last read it                      */
    u64 last_frame;
} rsx_vc_entry;

typedef struct rsx_vc_stats {
    u64 hits, misses, inserts, insert_failures, evictions, bytes_in_use;
} rsx_vc_stats;

typedef struct rsx_vertex_cache rsx_vertex_cache;

/* budget: total bytes of chunks; chunk_bytes: a power of two >= 64 KB,
 * halved (down to 64 KB) while it is larger than the budget. */
rsx_vertex_cache* rsx_vc_create(const rsx_vc_chunks* chunks, u64 budget,
                                u32 chunk_bytes);
void rsx_vc_destroy(rsx_vertex_cache* c);

/* The entry for key, or NULL. The pointer stays valid until the next insert. */
rsx_vc_entry* rsx_vc_find(rsx_vertex_cache* c, rsx_vc_key key);
/* Record that the entry is read by submission `serial` in `frame`. */
void rsx_vc_touch(rsx_vc_entry* e, u64 serial, u64 frame);

/* A new entry with a block of `bytes`; *cpu points at the block. `completed`
 * is the newest submission serial the GPU has finished, `frame` the current
 * frame: entries unused for a while are evicted when memory runs out. NULL
 * when the block cannot be had (larger than a chunk, or every byte is in use
 * by the GPU or by this frame); the caller then draws uncached. */
rsx_vc_entry* rsx_vc_insert(rsx_vertex_cache* c, rsx_vc_key key, u32 bytes,
                            u64 completed, u64 frame, u8** cpu);

/* The element range [first, last] a draw's indices span, remembered by a
 * hash of those indices, so a repeated draw finds its vertex range without
 * reading every index. */
int rsx_vc_range_find(rsx_vertex_cache* c, rsx_vc_key key, u32* first, u32* last);
void rsx_vc_range_put(rsx_vertex_cache* c, rsx_vc_key key, u32 first, u32 last);

void rsx_vc_get_stats(const rsx_vertex_cache* c, rsx_vc_stats* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_VERTEX_COMPACT_H */
