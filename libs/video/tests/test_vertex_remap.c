/*
 * rsx_vertex_remap_build (direct table when the references allow it) against
 * rsx_vertex_remap_build_hashed (always the hash table): same unique count,
 * same unique prefix of refs, same occurrence map.
 *
 * The two remap objects are reused across thousands of calls of varying
 * size, so a generation tag that survives from one draw into the next -- the
 * way a reused table goes wrong -- shows up as a reference wrongly reported
 * as already seen.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_remap libs/video/tests/test_vertex_remap.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x13198A2E03707344ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

int main(void)
{
    enum { RUNS = 20000, MAXN = 5000 };
    static rsx_vertex_ref in[MAXN], a[MAXN], b[MAXN];
    rsx_vertex_remap ra, rb;
    memset(&ra, 0, sizeof ra);
    memset(&rb, 0, sizeof rb);
    int direct_shaped = 0, mixed = 0, wide = 0;

    for (int n = 0; n < RUNS; n++) {
        const u32 count = 1 + rnd() % MAXN;
        const int shape = rnd() % 6;
        const u32 base = rnd() % 4 ? rnd() % 100u : 0;
        const u32 lo = rnd() % 70000u;
        const u32 span = shape == 0 ? 1 + rnd() % 8u           /* heavy duplication */
                       : shape == 1 ? 1u << 20                   /* exactly past the limit */
                       : shape == 2 ? 0xFFFFFFFFu - lo           /* anything */
                       : 1 + rnd() % 65536u;                     /* a 16-bit index buffer */
        for (u32 i = 0; i < count; i++) {
            in[i].vertex_id = lo + (span ? rnd() % span : 0);
            in[i].base_index = (shape == 3 && rnd() % 50 == 0) ? base + 1 : base;
        }
        if (shape == 3) mixed++;
        else if (shape == 1 || shape == 2) wide++;
        else direct_shaped++;

        memcpy(a, in, count * sizeof *in);
        memcpy(b, in, count * sizeof *in);
        u32 ua = 0, ub = 0;
        const int oka = rsx_vertex_remap_build(&ra, a, count, &ua);
        const int okb = rsx_vertex_remap_build_hashed(&rb, b, count, &ub);
        CHECK(oka && okb, "run %d: build failed (%d, %d)", n, oka, okb);
        CHECK(ua == ub, "run %d: unique %u vs %u", n, ua, ub);
        if (ua != ub) continue;
        CHECK(!memcmp(a, b, ua * sizeof *a), "run %d: unique prefix differs", n);
        for (u32 i = 0; i < count; i++)
            if (rsx_vertex_remap_index(&ra, i) != rsx_vertex_remap_index(&rb, i)) {
                CHECK(0, "run %d: occurrence %u maps to %u vs %u", n, i,
                      rsx_vertex_remap_index(&ra, i), rsx_vertex_remap_index(&rb, i));
                break;
            }
    }
    /* rsx_vertex_remap_build_range: applies exactly when every reference
     * shares a base and the id range fits in the reference count; then the
     * refs are lo..hi and every occurrence maps back to its own reference. */
    int range_ok = 0, range_no = 0;
    for (int n = 0; n < RUNS; n++) {
        const u32 count = 1 + rnd() % MAXN;
        const u32 lo = rnd() % 70000u;
        const u32 span = rnd() % 3 ? 1 + rnd() % count : 1 + rnd() % (2u * count);
        const u32 base = rnd() % 100u;
        const int mixed = rnd() % 10 == 0;
        u32 vlo = 0xFFFFFFFFu, vhi = 0;
        int bases_differ = 0;
        for (u32 i = 0; i < count; i++) {
            in[i].vertex_id = lo + rnd() % span;
            in[i].base_index = (mixed && rnd() % 7 == 0) ? base + 1 : base;
            if (in[i].vertex_id < vlo) vlo = in[i].vertex_id;
            if (in[i].vertex_id > vhi) vhi = in[i].vertex_id;
            bases_differ |= in[i].base_index != in[0].base_index;
        }
        const int expect = !bases_differ && (u64)vhi - vlo + 1u <= count;
        memcpy(a, in, count * sizeof *in);
        u32 out = 0;
        const int ok = rsx_vertex_remap_build_range(&ra, a, count, &out);
        CHECK(ok == expect, "range run %d: applied %d, expected %d", n, ok, expect);
        if (!ok) {
            range_no++;
            CHECK(!memcmp(a, in, count * sizeof *in), "range run %d: refs changed on refusal", n);
            continue;
        }
        range_ok++;
        CHECK(out == vhi - vlo + 1u, "range run %d: count %u, span %u", n, out, vhi - vlo + 1u);
        for (u32 i = 0; i < out; i++)
            if (a[i].vertex_id != vlo + i || a[i].base_index != in[0].base_index) {
                CHECK(0, "range run %d: ref %u is %u/%u", n, i, a[i].vertex_id, a[i].base_index);
                break;
            }
        for (u32 i = 0; i < count; i++) {
            const u32 m = rsx_vertex_remap_index(&ra, i);
            if (m >= out || a[m].vertex_id != in[i].vertex_id || a[m].base_index != in[i].base_index) {
                CHECK(0, "range run %d: occurrence %u maps to %u", n, i, m);
                break;
            }
        }
    }
    printf("test_vertex_remap: range mode %d applied, %d refused\n", range_ok, range_no);
    CHECK(range_ok > RUNS / 4 && range_no > RUNS / 10, "too few cases on one side of range mode");

    /* count 0 and 1 */
    u32 u = 99;
    CHECK(rsx_vertex_remap_build(&ra, a, 0, &u) && u == 0, "count 0");
    a[0].vertex_id = 7; a[0].base_index = 3;
    CHECK(rsx_vertex_remap_build(&ra, a, 1, &u) && u == 1 && rsx_vertex_remap_index(&ra, 0) == 0, "count 1");

    rsx_vertex_remap_destroy(&ra);
    rsx_vertex_remap_destroy(&rb);
    printf("test_vertex_remap: %d runs (%d direct-shaped, %d mixed base, %d wide span), %d failure(s)\n",
           RUNS, direct_shaped, mixed, wide, fails);
    return fails ? 1 : 0;
}
