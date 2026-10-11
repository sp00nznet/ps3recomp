/*
 * rsx_vertex_fetch_all against rsx_vertex_fetch_one, the per-vertex form it
 * replaces in the draw engine.
 *
 * Random fetch plans -- every vertex type (and invalid ones), sizes 0..5,
 * strides and offsets, frequency dividers in both modes, guest memory that
 * is partly unmapped, the stable-range path from _prepare and the per-element
 * resolver without it, and INLINE_ARRAY streams -- over random bytes, so NaN
 * payloads, denormal halves and SNORM -32768 all occur. The verdict and,
 * when it succeeds, every output byte must match.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch libs/video/tests/test_vertex_fetch.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

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

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline,
                        const u8* inl, u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        /* Weighted toward the formats a title uses; 0 = disabled, 8/9 invalid. */
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 7 ? 0 : r < 8 ? 8 + (rnd() & 1)
                     : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 4096u;
        f->desc.location = rnd() & 1u;
        for (int k = 0; k < 4; k++) {
            const u32 w = rnd();
            memcpy(&f->default_value[k], &w, 4);
        }
    }
    if (use_inline) {
        /* The shape rsx_vertex_fetch_plan_set_inline gives: offsets packed
         * in register order, one stride. */
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

int main(void)
{
    enum { PLANS = 6000, MAXV = 200 };
    static rsx_vertex_ref refs[MAXV];
    static u8 a[MAXV * 16 * 16], b[MAXV * 16 * 16];
    static u8 inl[8192];
    int ok_runs = 0, fail_runs = 0, inline_runs = 0, prepared = 0;

    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++) {
            for (u32 i = 0; i < GUEST; i++) guest[l][i] = (u8)rnd();
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        }
        for (u32 i = 0; i < sizeof inl; i++) inl[i] = (u8)rnd();
        const int use_inline = rnd() % 5 == 0;
        rsx_vertex_fetch_plan plan;
        random_plan(&plan, use_inline, inl, (u32)(rnd() % sizeof inl));

        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = (rnd() % 8 == 0) ? rnd() : rnd() % 300u;
            refs[i].base_index = base;
        }
        if (rnd() % 4 != 0) { rsx_vertex_fetch_plan_prepare(&plan, refs, count); prepared++; }
        if (use_inline) inline_runs++;

        const u32 stride = plan.layout.stride;
        memset(a, 0xAB, sizeof a);
        memset(b, 0xAB, sizeof b);
        int ra = 1;
        for (u32 i = 0; i < count && ra; i++)
            ra = rsx_vertex_fetch_one(&plan, &refs[i], stride ? a + (u64)i * stride : NULL);
        const int rb = rsx_vertex_fetch_all(&plan, refs, count, stride ? b : NULL);

        CHECK(ra == rb, "plan %d: fetch_one verdict %d, fetch_all %d", n, ra, rb);
        if (ra && rb) {
            ok_runs++;
            if (memcmp(a, b, (size_t)count * stride) != 0) {
                u32 off = 0;
                while (a[off] == b[off]) off++;
                const u32 v = stride ? off / stride : 0, slot = stride ? (off % stride) / 16u : 0;
                CHECK(0, "plan %d: output differs at vertex %u slot %u (attr %u type %u size %u)",
                      n, v, slot, plan.layout.attrs[slot],
                      plan.attr[plan.layout.attrs[slot]].desc.type,
                      plan.attr[plan.layout.attrs[slot]].desc.size);
            }
        } else if (!ra && !rb) {
            fail_runs++;
        }
    }

    printf("test_vertex_fetch: %d plans (%d succeeded, %d failed on both sides, %d prepared,"
           " %d inline), %d failure(s)\n", PLANS, ok_runs, fail_runs, prepared, inline_runs, fails);
    if (ok_runs < PLANS / 4 || fail_runs < 50 || inline_runs < 200) {
        printf("FAIL: too few cases exercised a path\n");
        return 1;
    }
    return fails ? 1 : 0;
}
