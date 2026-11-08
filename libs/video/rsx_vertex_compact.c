#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_default(const rsx_dispatch* rsx, u32 attr, float out[4])
{
    rsx_dsp_vertex_default(rsx, attr, out);
    if (attr != 3)
        return;
    const u32 base = (0x1C00u + attr * 0x10u) >> 2;
    const u32 written = rsx->seen[base] | rsx->seen[base + 1] |
        rsx->seen[base + 2] | rsx->seen[base + 3];
    /* Preserve the legacy white color fallback only for an unwritten
     * attribute. An explicit (0,0,0,1) is shader input, even when it equals
     * the hardware default; a shader may swizzle x into output alpha. */
    if (!written && out[0] == 0.0f && out[1] == 0.0f &&
        out[2] == 0.0f && out[3] == 1.0f)
        out[0] = out[1] = out[2] = 1.0f;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_vertex_fetch_default(rsx, attr, fetch->default_value);
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (rsx_vertex_freq_plain) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (rsx_vertex_freq_plain(fetch->desc.frequency,
                                  (plan->divider_mask >> attr) & 1u)) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            rsx_dsp_resolve_vertex_offset(plan->base_offset,
                                          fetch->desc.offset) +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

/* The stable-range loop for one attribute and one vertex type: `decode`
 * reads `src` into `v` (pre-set to 0,0,0,1) for components c < n. Elements
 * outside the stable range take the one-vertex path. */
#define RSX_FETCH_LOOP(decode)                                                  \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                          \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;      \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        if (!type || !fetch->desc.size || !base ||
            type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
            break;
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
            break;
        default:   /* RSX_VTX_TYPE_CMP32: one packed word, whatever the size */
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}

/* ---- converted-vertex cache ---------------------------------------------- */

/* The hash: four independent lanes, each folding 16 bytes per step with a
 * 64x64->128 multiply whose halves are xor-ed together, the "mum" step of
 * wyhash (Wang Yi, public domain / Unlicense). The constants are wyhash's
 * default secret. Lanes keep the multiplies independent, so a frame's tens of
 * megabytes of vertex data hash at memory speed. The key is 128 bits, but a
 * difference in the input reaches it through one 64-bit lane: two different
 * inputs share a key with a probability of about 2^-64. */
#define VC_S0 0xa0761d6478bd642full
#define VC_S1 0xe7037ed1a0b428dbull
#define VC_S2 0x8ebc6af09c88c6e3ull
#define VC_S3 0x589965cc75374cc3ull

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static u64 vc_mum(u64 a, u64 b)
{
#if defined(_M_X64)
    u64 hi;
    const u64 lo = _umul128(a, b, &hi);
    return lo ^ hi;
#else
    return (a * b) ^ __umulh(a, b);
#endif
}
#else
static u64 vc_mum(u64 a, u64 b)
{
    const __uint128_t r = (__uint128_t)a * b;
    return (u64)r ^ (u64)(r >> 64);
}
#endif

static u64 vc_rd64(const u8* p)
{
    u64 v;
    memcpy(&v, p, sizeof v);
    return v;
}

rsx_vc_key rsx_vc_hash(const void* data, size_t n, rsx_vc_key seed)
{
    const u8* p = (const u8*)data;
    const u64 len = (u64)n;
    u64 a = seed.lo ^ VC_S0, b = seed.hi ^ VC_S1;
    u64 c = seed.lo ^ VC_S2 ^ len, d = seed.hi ^ VC_S3;
    while (n >= 64) {
        a = vc_mum(vc_rd64(p)      ^ VC_S0, vc_rd64(p + 8)  ^ a);
        b = vc_mum(vc_rd64(p + 16) ^ VC_S1, vc_rd64(p + 24) ^ b);
        c = vc_mum(vc_rd64(p + 32) ^ VC_S2, vc_rd64(p + 40) ^ c);
        d = vc_mum(vc_rd64(p + 48) ^ VC_S3, vc_rd64(p + 56) ^ d);
        p += 64;
        n -= 64;
    }
    while (n >= 16) {
        a = vc_mum(vc_rd64(p) ^ VC_S1, vc_rd64(p + 8) ^ a);
        p += 16;
        n -= 16;
    }
    u8 tail[16] = {0};
    if (n) memcpy(tail, p, n);
    b = vc_mum(vc_rd64(tail) ^ VC_S2, vc_rd64(tail + 8) ^ b ^ (u64)n);
    rsx_vc_key out;
    const u64 x = vc_mum(a ^ VC_S1, b ^ len ^ VC_S3);
    const u64 y = vc_mum(c ^ VC_S0, d ^ VC_S2);
    out.lo = vc_mum(x ^ VC_S2, y ^ VC_S1);
    out.hi = vc_mum(x ^ VC_S3, y ^ seed.hi ^ VC_S0) ^ out.lo;
    return out;
}

rsx_vc_key rsx_vc_mix(rsx_vc_key k, u64 v)
{
    return rsx_vc_hash(&v, sizeof v, k);
}

/* Blocks are multiples of 256 bytes, so every offset is aligned for any
 * vertex or index buffer a host API binds. */
#define VC_MIN_SHIFT   8u
#define VC_MIN_CHUNK   (64u << 10)
#define VC_TAG_FREE    0x80u
#define VC_MAX_ENTRIES 32768u
/* Ranges: 4-way sets, the least recently stored way replaced. A firefight
 * frame has ~7800 distinct index keys; direct-mapped at 16K slots a fifth of
 * them collided every frame and their draws converted again. */
#define VC_RANGES      65536u
#define VC_RANGE_WAYS  4u
/* Entries unused for this many frames go first when memory runs out; past
 * that, anything not used this frame or the last. */
#define VC_AGE_OLD     30u
#define VC_AGE_MIN     2u

typedef struct {
    u32 handle;
    u8* cpu;
    u8* tag;           /* per granule at a block start: order + 1 | FREE */
} vc_chunk;

typedef struct {
    u32 chunk, granule;
    u8 order;
    u64 serial;
} vc_deferred;

typedef struct {
    rsx_vc_key key;
    u32 first, last;
    u64 stamp;          /* 0 = empty */
} vc_range;

struct rsx_vertex_cache {
    rsx_vc_chunks be;
    u32 chunk_bytes, max_chunks, max_order;
    vc_chunk* chunks;
    u32 n_chunks;

    /* Free blocks per order, as (chunk << 32 | granule). An entry is only a
     * hint: it stands for a free block while the tag there still says so. */
    u64** free_list;
    u32* free_n;
    u32* free_cap;

    vc_deferred* deferred;
    u32 n_deferred, cap_deferred;
    u64 deferred_min;        /* smallest serial among them, while there are any */

    rsx_vc_entry* entries;   /* VC_MAX_ENTRIES */
    u8* live;
    u32* entry_free;
    u32 n_entry_free;
    u32* slots;              /* 2 * VC_MAX_ENTRIES; entry index + 1, 0 empty */
    u32 slot_mask;

    vc_range* ranges;
    u64 range_stamp;
    u64 last_failed_scan_frame;
    rsx_vc_stats stats;
};

static int vc_push_free(rsx_vertex_cache* c, u32 order, u32 chunk, u32 granule)
{
    if (c->free_n[order] == c->free_cap[order]) {
        const u32 cap = c->free_cap[order] ? c->free_cap[order] * 2u : 64u;
        u64* p = (u64*)realloc(c->free_list[order], (size_t)cap * sizeof(u64));
        if (!p) return 0;
        c->free_list[order] = p;
        c->free_cap[order] = cap;
    }
    c->free_list[order][c->free_n[order]++] = ((u64)chunk << 32) | granule;
    return 1;
}

static int vc_chunk_add(rsx_vertex_cache* c)
{
    if (c->n_chunks >= c->max_chunks || !c->be.create) return 0;
    const u32 granules = c->chunk_bytes >> VC_MIN_SHIFT;
    u8* tag = (u8*)calloc(granules, 1);
    if (!tag) return 0;
    u8* cpu = NULL;
    const u32 handle = c->be.create(c->be.user, c->chunk_bytes, &cpu);
    if (!handle || !cpu) { free(tag); return 0; }
    vc_chunk* ch = &c->chunks[c->n_chunks];
    ch->handle = handle;
    ch->cpu = cpu;
    ch->tag = tag;
    tag[0] = (u8)(VC_TAG_FREE | (c->max_order + 1u));
    if (!vc_push_free(c, c->max_order, c->n_chunks, 0)) {
        if (c->be.release) c->be.release(c->be.user, handle);
        free(tag);
        return 0;
    }
    c->n_chunks++;
    return 1;
}

/* Pop a block of exactly `order` that is still free, or -1. */
static int vc_pop_free(rsx_vertex_cache* c, u32 order, u32* chunk, u32* granule)
{
    const u8 want = (u8)(VC_TAG_FREE | (order + 1u));
    while (c->free_n[order]) {
        const u64 v = c->free_list[order][--c->free_n[order]];
        const u32 ci = (u32)(v >> 32), g = (u32)v;
        if (c->chunks[ci].tag[g] == want) {
            /* Mark it taken now: a stale duplicate of this entry further
             * down the list must not hand the same block out twice. */
            c->chunks[ci].tag[g] = (u8)(order + 1u);
            *chunk = ci;
            *granule = g;
            return 0;
        }
    }
    return -1;
}

static int vc_alloc(rsx_vertex_cache* c, u32 order, u32* chunk, u32* granule)
{
    for (int pass = 0; pass < 2; pass++) {
        for (u32 o = order; o <= c->max_order; o++) {
            u32 ci, g;
            if (vc_pop_free(c, o, &ci, &g) != 0) continue;
            u8* tag = c->chunks[ci].tag;
            while (o > order) {
                o--;
                const u32 buddy = g + (1u << o);
                /* Tagged free even when its list cannot take it: vc_free
                 * merges by the tags, so the block comes back when its
                 * buddy is freed. */
                tag[buddy] = (u8)(VC_TAG_FREE | (o + 1u));
                vc_push_free(c, o, ci, buddy);
            }
            tag[g] = (u8)(order + 1u);
            *chunk = ci;
            *granule = g;
            return 1;
        }
        if (pass == 0 && !vc_chunk_add(c)) break;
    }
    return 0;
}

static void vc_free(rsx_vertex_cache* c, u32 ci, u32 g, u32 order)
{
    u8* tag = c->chunks[ci].tag;
    tag[g] = 0;
    while (order < c->max_order) {
        const u32 buddy = g ^ (1u << order);
        if (tag[buddy] != (u8)(VC_TAG_FREE | (order + 1u))) break;
        tag[buddy] = 0;
        if (buddy < g) g = buddy;
        order++;
    }
    tag[g] = (u8)(VC_TAG_FREE | (order + 1u));
    vc_push_free(c, order, ci, g);
}

static void vc_reclaim(rsx_vertex_cache* c, u64 completed)
{
    /* Every insert asks; a walk only pays once the oldest reader is done. */
    if (!c->n_deferred || completed < c->deferred_min) return;
    u32 keep = 0;
    u64 min = ~0ull;
    for (u32 i = 0; i < c->n_deferred; i++) {
        const vc_deferred d = c->deferred[i];
        if (d.serial <= completed) {
            vc_free(c, d.chunk, d.granule, d.order);
        } else {
            c->deferred[keep++] = d;
            if (d.serial < min) min = d.serial;
        }
    }
    c->n_deferred = keep;
    c->deferred_min = min;
}

static u32 vc_slot_of(const rsx_vertex_cache* c, rsx_vc_key key)
{
    return (u32)(key.lo ^ (key.hi >> 17)) & c->slot_mask;
}

static int vc_key_eq(rsx_vc_key a, rsx_vc_key b)
{
    return a.lo == b.lo && a.hi == b.hi;
}

/* Remove the table slot holding entry index `ei` (linear probing, backward
 * shift so no tombstones are left). */
static void vc_slot_remove(rsx_vertex_cache* c, u32 ei)
{
    u32 s = vc_slot_of(c, c->entries[ei].key);
    while (c->slots[s] != ei + 1u) s = (s + 1u) & c->slot_mask;
    for (;;) {
        c->slots[s] = 0;
        u32 n = s;
        for (;;) {
            n = (n + 1u) & c->slot_mask;
            if (!c->slots[n]) return;
            const u32 home = vc_slot_of(c, c->entries[c->slots[n] - 1u].key);
            /* Move n back into the hole at s unless its home lies in (s, n]. */
            const int in_range = s <= n ? (home > s && home <= n)
                                        : (home > s || home <= n);
            if (!in_range) break;
        }
        c->slots[s] = c->slots[n];
        s = n;
    }
}

/* Drop one entry: its block is freed now when the GPU is done with it, or
 * once it is. */
static void vc_evict(rsx_vertex_cache* c, u32 ei, u64 completed)
{
    rsx_vc_entry* e = &c->entries[ei];
    vc_slot_remove(c, ei);
    c->live[ei] = 0;
    c->entry_free[c->n_entry_free++] = ei;
    c->stats.evictions++;
    c->stats.bytes_in_use -= (u64)1 << (e->order + VC_MIN_SHIFT);
    u32 ci = 0;
    while (ci < c->n_chunks && c->chunks[ci].handle != e->chunk) ci++;
    if (ci == c->n_chunks) return;
    const u32 g = e->offset >> VC_MIN_SHIFT;
    if (e->last_serial <= completed) {
        vc_free(c, ci, g, e->order);
        return;
    }
    if (c->n_deferred == c->cap_deferred) {
        const u32 cap = c->cap_deferred ? c->cap_deferred * 2u : 256u;
        vc_deferred* p = (vc_deferred*)realloc(c->deferred, (size_t)cap * sizeof(*p));
        if (!p) return;   /* the block leaks; better than a GPU reading reused memory */
        c->deferred = p;
        c->cap_deferred = cap;
    }
    c->deferred[c->n_deferred].chunk = ci;
    c->deferred[c->n_deferred].granule = g;
    c->deferred[c->n_deferred].order = e->order;
    c->deferred[c->n_deferred].serial = e->last_serial;
    if (!c->n_deferred || e->last_serial < c->deferred_min) c->deferred_min = e->last_serial;
    c->n_deferred++;
}

static u32 vc_evict_older(rsx_vertex_cache* c, u64 frame, u32 age, u64 completed)
{
    u32 n = 0;
    for (u32 i = 0; i < VC_MAX_ENTRIES; i++)
        if (c->live[i] && frame >= c->entries[i].last_frame + age) {
            vc_evict(c, i, completed);
            n++;
        }
    return n;
}

rsx_vertex_cache* rsx_vc_create(const rsx_vc_chunks* chunks, u64 budget,
                                u32 chunk_bytes)
{
    if (!chunks || !chunks->create || chunk_bytes < VC_MIN_CHUNK ||
        (chunk_bytes & (chunk_bytes - 1u)) || chunk_bytes > (1u << 30))
        return NULL;
    rsx_vertex_cache* c = (rsx_vertex_cache*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->be = *chunks;
    /* A budget under one chunk gets smaller chunks, not one past it. */
    while (chunk_bytes > budget && chunk_bytes > VC_MIN_CHUNK) chunk_bytes >>= 1;
    c->chunk_bytes = chunk_bytes;
    c->max_chunks = (u32)(budget / chunk_bytes);
    if (!c->max_chunks) c->max_chunks = 1;
    u32 order = 0;
    while ((1u << (order + VC_MIN_SHIFT)) < chunk_bytes) order++;
    c->max_order = order;
    c->chunks = (vc_chunk*)calloc(c->max_chunks, sizeof(vc_chunk));
    c->free_list = (u64**)calloc(order + 1u, sizeof(u64*));
    c->free_n = (u32*)calloc(order + 1u, sizeof(u32));
    c->free_cap = (u32*)calloc(order + 1u, sizeof(u32));
    c->entries = (rsx_vc_entry*)calloc(VC_MAX_ENTRIES, sizeof(rsx_vc_entry));
    c->live = (u8*)calloc(VC_MAX_ENTRIES, 1);
    c->entry_free = (u32*)malloc(VC_MAX_ENTRIES * sizeof(u32));
    c->slots = (u32*)calloc(2u * VC_MAX_ENTRIES, sizeof(u32));
    c->ranges = (vc_range*)calloc(VC_RANGES, sizeof(vc_range));
    if (!c->chunks || !c->free_list || !c->free_n || !c->free_cap || !c->entries ||
        !c->live || !c->entry_free || !c->slots || !c->ranges) {
        rsx_vc_destroy(c);
        return NULL;
    }
    c->slot_mask = 2u * VC_MAX_ENTRIES - 1u;
    for (u32 i = 0; i < VC_MAX_ENTRIES; i++)
        c->entry_free[i] = VC_MAX_ENTRIES - 1u - i;
    c->n_entry_free = VC_MAX_ENTRIES;
    c->last_failed_scan_frame = ~0ull;
    return c;
}

void rsx_vc_destroy(rsx_vertex_cache* c)
{
    if (!c) return;
    for (u32 i = 0; i < c->n_chunks; i++) {
        if (c->be.release) c->be.release(c->be.user, c->chunks[i].handle);
        free(c->chunks[i].tag);
    }
    if (c->free_list)
        for (u32 o = 0; o <= c->max_order; o++) free(c->free_list[o]);
    free(c->free_list); free(c->free_n); free(c->free_cap);
    free(c->chunks); free(c->deferred);
    free(c->entries); free(c->live); free(c->entry_free); free(c->slots);
    free(c->ranges);
    free(c);
}

rsx_vc_entry* rsx_vc_find(rsx_vertex_cache* c, rsx_vc_key key)
{
    if (!c) return NULL;
    for (u32 s = vc_slot_of(c, key); c->slots[s]; s = (s + 1u) & c->slot_mask) {
        rsx_vc_entry* e = &c->entries[c->slots[s] - 1u];
        if (vc_key_eq(e->key, key)) {
            c->stats.hits++;
            return e;
        }
    }
    c->stats.misses++;
    return NULL;
}

void rsx_vc_touch(rsx_vc_entry* e, u64 serial, u64 frame)
{
    if (!e) return;
    if (serial > e->last_serial) e->last_serial = serial;
    e->last_frame = frame;
}

rsx_vc_entry* rsx_vc_insert(rsx_vertex_cache* c, rsx_vc_key key, u32 bytes,
                            u64 completed, u64 frame, u8** cpu)
{
    if (!c || !bytes || bytes > c->chunk_bytes || !cpu) return NULL;
    u32 order = 0;
    while ((1ull << (order + VC_MIN_SHIFT)) < bytes) order++;

    /* An entry already under this key (the caller missed it) goes first. */
    for (u32 s = vc_slot_of(c, key); c->slots[s]; s = (s + 1u) & c->slot_mask)
        if (vc_key_eq(c->entries[c->slots[s] - 1u].key, key)) {
            vc_evict(c, c->slots[s] - 1u, completed);
            break;
        }

    vc_reclaim(c, completed);
    u32 ci = 0, g = 0;
    int ok = c->n_entry_free && vc_alloc(c, order, &ci, &g);
    if (!ok && c->last_failed_scan_frame != frame) {
        static const u32 ages[2] = { VC_AGE_OLD, VC_AGE_MIN };
        for (u32 a = 0; a < 2 && !ok; a++) {
            if (!vc_evict_older(c, frame, ages[a], completed)) continue;
            vc_reclaim(c, completed);
            ok = c->n_entry_free && vc_alloc(c, order, &ci, &g);
        }
        /* Nothing more can be had until the frame changes: do not scan
         * every entry again on each miss of this frame. */
        if (!ok) c->last_failed_scan_frame = frame;
    }
    if (!ok) {
        c->stats.insert_failures++;
        return NULL;
    }

    const u32 ei = c->entry_free[--c->n_entry_free];
    rsx_vc_entry* e = &c->entries[ei];
    memset(e, 0, sizeof *e);
    e->key = key;
    e->chunk = c->chunks[ci].handle;
    e->offset = g << VC_MIN_SHIFT;
    e->order = (u8)order;
    e->last_frame = frame;
    c->live[ei] = 1;
    u32 s = vc_slot_of(c, key);
    while (c->slots[s]) s = (s + 1u) & c->slot_mask;
    c->slots[s] = ei + 1u;
    c->stats.inserts++;
    c->stats.bytes_in_use += (u64)1 << (order + VC_MIN_SHIFT);
    *cpu = c->chunks[ci].cpu + e->offset;
    return e;
}

static vc_range* vc_range_set(rsx_vertex_cache* c, rsx_vc_key key)
{
    return &c->ranges[((key.lo ^ key.hi) & (VC_RANGES / VC_RANGE_WAYS - 1u)) * VC_RANGE_WAYS];
}

int rsx_vc_range_find(rsx_vertex_cache* c, rsx_vc_key key, u32* first, u32* last)
{
    if (!c) return 0;
    vc_range* set = vc_range_set(c, key);
    for (u32 w = 0; w < VC_RANGE_WAYS; w++)
        if (set[w].stamp && vc_key_eq(set[w].key, key)) {
            set[w].stamp = ++c->range_stamp;
            *first = set[w].first;
            *last = set[w].last;
            return 1;
        }
    return 0;
}

void rsx_vc_range_put(rsx_vertex_cache* c, rsx_vc_key key, u32 first, u32 last)
{
    if (!c) return;
    vc_range* set = vc_range_set(c, key);
    vc_range* r = &set[0];
    for (u32 w = 0; w < VC_RANGE_WAYS; w++) {
        if (set[w].stamp && vc_key_eq(set[w].key, key)) { r = &set[w]; break; }
        if (set[w].stamp < r->stamp) r = &set[w];
    }
    r->key = key;
    r->first = first;
    r->last = last;
    r->stamp = ++c->range_stamp;
}

void rsx_vc_get_stats(const rsx_vertex_cache* c, rsx_vc_stats* out)
{
    if (!out) return;
    if (!c) { memset(out, 0, sizeof *out); return; }
    *out = c->stats;
}
