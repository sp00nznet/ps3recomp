/*
 * rsx_vertex_fetch_plan_prepare against the per-attribute walk it replaced.
 *
 * _prepare finds the span of ordinary per-vertex attributes (rsx_vertex_freq_plain)
 * once per draw instead of once per attribute. Random fetch plans -- every
 * vertex type, strides and offsets, frequency dividers in both modes, guest
 * memory that is partly unmapped, INLINE_ARRAY streams -- with random index
 * references, some past the 20-bit element wrap: every attribute's stable
 * range (base, first, last) must be what the old walk gave, and
 * rsx_vertex_fetch_many must produce exactly the bytes and the result of
 * rsx_vertex_fetch_one over the same references.
 *
 * Also: VERTEX_DATA_BASE_OFFSET + an array's offset wraps at 28 bits, for
 * both the stable span and the per-element fetch.
 *
 * Also: frequency 1 under a modulo operation bit reads element 0 for every
 * vertex (vertex % 1), while frequency 1 under division stays per vertex.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch_plan libs/video/tests/test_vertex_fetch_plan.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 16 };
static uint8_t guest[2][GUEST];         /* two locations */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

/* The walk _prepare used to do: every reference, once per attribute. */
static void ref_prepare(rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;
        if (plan->inline_data) {
            const u32 stride = plan->inline_stride ? plan->inline_stride : fetch->stride;
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
        u32 first = UINT_MAX, last = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index, fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        /* base + array offset wraps at 28 bits (rsx_dispatch.h) */
        const u64 prefix = (plan->base_offset + fetch->desc.offset) & 0x0FFFFFFFu;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end = prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start || end - start > UINT_MAX)
            continue;
        const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                         (u32)start, (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline,
                        const u8* inl, u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    if (rnd() % 8 == 0) plan->base_offset += (1u + rnd() % 15u) << 28;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 7 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 4096u;
        f->desc.location = rnd() & 1u;
    }
    if (use_inline) {
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = inline_packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                inline_packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = inline_packed ? inline_packed : 16u;
        plan->inline_data = inl;
        plan->inline_bytes = inl_bytes;
    }
}

/* Resistance: Fall of Man's world lighting stream (UB256 x4, stride 4) sits
 * at VERTEX_DATA_BASE_OFFSET + array offset = 0x1Fxxxxxx, past the end of
 * local memory; wrapped at 28 bits it is local 0x0Fxxxxxx. Here the sum is
 * 0x10000100, which must read local 0x100, through the stable span and
 * through the per-element fetch. */
static void test_offset_wraps_at_28_bits(void)
{
    memset(guest, 0, sizeof guest);
    mapped_end[0] = mapped_end[1] = GUEST;
    const u32 local = 0x0100;
    for (u32 v = 0; v < 4; v++)
        for (u32 c = 0; c < 4; c++) guest[0][local + v * 4 + c] = (u8)(10 * v + c + 1);

    for (int stable = 0; stable < 2; stable++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u << 4);
        plan.base_offset = 0x0D9272C0u;
        plan.guest_ptr = guest_ptr;
        rsx_vertex_fetch_attr* f = &plan.attr[4];
        f->desc.type = RSX_VTX_TYPE_UINT8;
        f->desc.size = 4;
        f->desc.stride = 4;
        f->desc.location = 0;
        f->desc.offset = 0x10000000u + local - plan.base_offset;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->stride = 4;
        rsx_vertex_ref refs[4];
        for (u32 v = 0; v < 4; v++) { refs[v].vertex_id = v; refs[v].base_index = 0; }
        if (stable) {
            rsx_vertex_fetch_plan_prepare(&plan, refs, 4);
            CHECK(plan.attr[4].stable_base == guest[0] + local,
                  "stable span not at the wrapped offset");
        }
        for (u32 v = 0; v < 4; v++) {
            float out[4] = {0, 0, 0, 0};
            CHECK(rsx_vertex_fetch_one(&plan, &refs[v], (u8*)out), "fetch %u failed", v);
            CHECK(out[0] == 10.0f * v + 1 && out[3] == 10.0f * v + 4,
                  "%s fetch vertex %u read %g..%g", stable ? "stable" : "per-element",
                  v, out[0], out[3]);
        }
    }
}

static void put_be_f32(u8* p, float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    p[0] = (u8)(u >> 24); p[1] = (u8)(u >> 16); p[2] = (u8)(u >> 8); p[3] = (u8)u;
}

/* Attribute 0 holds elements (10,11,12), (20,21,22), ...: frequency 1 with
 * modulo gives every vertex element 0; with division, vertex i element i. */
static void test_modulo_frequency_one(void)
{
    enum { VERTS = 4 };
    mapped_end[0] = GUEST;
    for (u32 e = 0; e < VERTS; e++)
        for (u32 c = 0; c < 3; c++)
            put_be_f32(&guest[0][256 + e * 12 + c * 4], (float)(10 * (e + 1) + c));
    rsx_vertex_ref refs[VERTS];
    for (u32 i = 0; i < VERTS; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }

    for (u32 modulo = 0; modulo < 2; modulo++) {
        rsx_vertex_fetch_plan plan;
        memset(&plan, 0, sizeof plan);
        rsx_vertex_layout_plan_init(&plan.layout, 1u);
        plan.divider_mask = modulo;
        plan.guest_ptr = guest_ptr;
        rsx_vertex_fetch_attr* f = &plan.attr[0];
        f->desc.type = RSX_VTX_TYPE_FLOAT;
        f->desc.size = 3;
        f->desc.stride = 12;
        f->desc.frequency = 1;
        f->desc.offset = 256;
        f->elem_size = 12;
        f->stride = 12;
        rsx_vertex_fetch_plan_prepare(&plan, refs, VERTS);
        CHECK(f->stable_base && f->stable_first == 0 &&
              f->stable_last == (modulo ? 0u : VERTS - 1u),
              "modulo %u: stable range %u..%u", modulo, f->stable_first, f->stable_last);
        for (u32 i = 0; i < VERTS; i++) {
            const float want = modulo ? 10.0f : (float)(10 * (i + 1));
            float one[4];
            CHECK(rsx_vertex_fetch_one(&plan, &refs[i], (u8*)one) &&
                  one[0] == want && one[2] == want + 2.0f,
                  "modulo %u vertex %u: fetch_one (%g,%g,%g), want x %g", modulo, i,
                  one[0], one[1], one[2], want);
        }
    }
}

int main(void)
{
    test_offset_wraps_at_28_bits();
    test_modulo_frequency_one();

    enum { PLANS = 6000, MAXV = 200 };
    static rsx_vertex_ref refs[MAXV];
    static u8 inl[8192];
    int stable = 0, divided = 0, fetched = 0;
    for (u32 i = 0; i < GUEST; i++) { guest[0][i] = (u8)rnd(); guest[1][i] = (u8)rnd(); }
    for (u32 i = 0; i < sizeof inl; i++) inl[i] = (u8)rnd();

    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan plan, ref;
        random_plan(&plan, rnd() % 5 == 0, inl, (u32)(rnd() % sizeof inl));

        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        /* Some runs sit just below the 20-bit element wrap and cross it. */
        const u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = start + ((rnd() % 8 == 0) ? rnd() : rnd() % 300u);
            refs[i].base_index = base;
        }
        ref = plan;
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        ref_prepare(&ref, refs, count);
        /* The batched fetch against one vertex at a time. */
        {
            static u8 one[MAXV * 256], many[MAXV * 256];
            const u32 stride = plan.layout.stride;
            int ok_one = 1;
            memset(one, 0xCD, sizeof one);
            memset(many, 0xCD, sizeof many);
            for (u32 i = 0; i < count && ok_one; i++)
                ok_one = rsx_vertex_fetch_one(&plan, &refs[i], one + (size_t)i * stride);
            const int ok_many = rsx_vertex_fetch_many(&plan, refs, count, many);
            CHECK(ok_one == ok_many, "plan %d: fetch_many %d, fetch_one %d", n, ok_many, ok_one);
            CHECK(!ok_one || !memcmp(one, many, (size_t)count * stride),
                  "plan %d: fetch_many vertices differ from fetch_one", n);
            if (ok_one) fetched++;
        }
        for (u32 at = 0; at < RSX_DSP_NUM_VERTEX_ATTR; at++) {
            CHECK(plan.attr[at].stable_base == ref.attr[at].stable_base &&
                  plan.attr[at].stable_first == ref.attr[at].stable_first &&
                  plan.attr[at].stable_last == ref.attr[at].stable_last &&
                  plan.attr[at].stride == ref.attr[at].stride,
                  "plan %d attr %u: span %u..%u, walk %u..%u", n, at,
                  plan.attr[at].stable_first, plan.attr[at].stable_last,
                  ref.attr[at].stable_first, ref.attr[at].stable_last);
            if (ref.attr[at].stable_base) {
                stable++;
                if (plan.attr[at].desc.frequency > 1) divided++;
            }
        }
    }

    printf("test_vertex_fetch_plan: %d plans, %d stable ranges (%d divided), %d fetched, %d failure(s)\n",
           PLANS, stable, divided, fetched, fails);
    if (stable < PLANS || divided < 100 || fetched < PLANS / 2) {
        printf("FAIL: too few cases exercised a path\n");
        return 1;
    }
    return fails ? 1 : 0;
}
