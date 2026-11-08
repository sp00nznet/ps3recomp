/* The converted-vertex cache (rsx_vertex_compact.h) without a GPU: the hash,
 * lookups, the buddy allocator under its chunks, eviction by age and the rule
 * that a block the GPU may still read is not handed out again.
 *
 * clang -std=gnu17 -O2 -Wall -Wextra -I include -I libs/video \
 *   libs/video/tests/test_vertex_cache.c libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c */
#include "rsx_vertex_compact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("[FAIL] " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

#define CHUNK (64u << 10)

typedef struct { int created, released; u32 bytes; u8* mem[16]; } Chunks;
static Chunks g_ch;

static u32 ch_create(void* u, u32 bytes, u8** cpu)
{
    Chunks* c = (Chunks*)u;
    if (c->created >= 16) return 0;
    u8* p = (u8*)calloc(1, bytes);
    if (!p) return 0;
    c->bytes = bytes;
    c->mem[c->created] = p;
    *cpu = p;
    return (u32)++c->created;
}

static void ch_release(void* u, u32 chunk)
{
    Chunks* c = (Chunks*)u;
    c->released++;
    free(c->mem[chunk - 1]);
    c->mem[chunk - 1] = NULL;
}

static const rsx_vc_chunks k_chunks = { &g_ch, ch_create, ch_release };

static rsx_vc_key key_of(u32 v)
{
    const rsx_vc_key zero = { 0, 0 };
    return rsx_vc_hash(&v, sizeof v, zero);
}

static void test_hash(void)
{
    const rsx_vc_key s = { 1, 2 };
    u8 a[200], b[200];
    for (u32 i = 0; i < sizeof a; i++) a[i] = b[i] = (u8)(i * 7u);
    for (u32 n = 0; n <= sizeof a; n++) {
        const rsx_vc_key x = rsx_vc_hash(a, n, s), y = rsx_vc_hash(b, n, s);
        CHECK(x.lo == y.lo && x.hi == y.hi, "same bytes, same key (n=%u)", n);
        if (n) {
            b[n - 1] ^= 1u;
            const rsx_vc_key z = rsx_vc_hash(b, n, s);
            CHECK(z.lo != x.lo && z.hi != x.hi, "a flipped last bit changes both halves (n=%u)", n);
            b[n - 1] ^= 1u;
            b[0] ^= 0x80u;
            const rsx_vc_key w = rsx_vc_hash(b, n, s);
            CHECK(w.lo != x.lo && w.hi != x.hi, "a flipped first bit changes both halves (n=%u)", n);
            b[0] ^= 0x80u;
        }
    }
    /* Zero bytes of different lengths and different seeds are different keys. */
    const u8 zero[64] = {0};
    const rsx_vc_key z1 = rsx_vc_hash(zero, 32, s), z2 = rsx_vc_hash(zero, 33, s);
    CHECK(z1.lo != z2.lo, "length is part of the key");
    const rsx_vc_key s2 = { 1, 3 };
    const rsx_vc_key z3 = rsx_vc_hash(zero, 32, s2);
    CHECK(z1.lo != z3.lo, "seed is part of the key");
    const rsx_vc_key m1 = rsx_vc_mix(s, 5), m2 = rsx_vc_mix(s, 6);
    CHECK(m1.lo != m2.lo, "mix of different values differs");
}

static void test_find_insert(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    rsx_vertex_cache* c = rsx_vc_create(&k_chunks, 2u * CHUNK, CHUNK);
    CHECK(c != NULL, "create");
    CHECK(rsx_vc_find(c, key_of(1)) == NULL, "empty cache misses");
    u8* cpu = NULL;
    rsx_vc_entry* e = rsx_vc_insert(c, key_of(1), 1000, 0, 0, &cpu);
    CHECK(e && cpu, "insert");
    CHECK(e && e->offset % 256u == 0, "blocks are 256-byte aligned");
    memset(cpu, 0xAB, 1000);
    rsx_vc_entry* f = rsx_vc_find(c, key_of(1));
    CHECK(f == e, "inserted entry is found");
    CHECK(g_ch.mem[0] && g_ch.mem[0][f->offset] == 0xAB, "the block is the chunk's memory");
    CHECK(rsx_vc_find(c, key_of(2)) == NULL, "another key misses");
    CHECK(rsx_vc_insert(c, key_of(3), CHUNK + 1u, 0, 0, &cpu) == NULL, "larger than a chunk: refused");
    /* Inserting a key again replaces the entry. */
    rsx_vc_entry* g2 = rsx_vc_insert(c, key_of(1), 300, 0, 0, &cpu);
    CHECK(g2 && rsx_vc_find(c, key_of(1)) == g2, "re-insert replaces");
    rsx_vc_stats st;
    rsx_vc_get_stats(c, &st);
    CHECK(st.inserts == 2 && st.evictions == 1, "stats count inserts/evictions (%llu/%llu)",
          (unsigned long long)st.inserts, (unsigned long long)st.evictions);
    /* Ranges. */
    u32 a, b;
    CHECK(!rsx_vc_range_find(c, key_of(9), &a, &b), "unknown range misses");
    rsx_vc_range_put(c, key_of(9), 5, 77);
    CHECK(rsx_vc_range_find(c, key_of(9), &a, &b) && a == 5 && b == 77, "range found");
    rsx_vc_destroy(c);
    CHECK(g_ch.created == g_ch.released, "destroy releases every chunk");
}

/* One chunk, filled; nothing may be reused while the GPU still reads it. */
static void test_deferred_reuse(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    rsx_vertex_cache* c = rsx_vc_create(&k_chunks, CHUNK, CHUNK);
    u8* cpu;
    rsx_vc_entry* e = rsx_vc_insert(c, key_of(1), CHUNK, 0, 0, &cpu);
    CHECK(e != NULL, "whole-chunk insert");
    rsx_vc_touch(e, 5, 0);                     /* read by submission 5 */
    CHECK(rsx_vc_insert(c, key_of(2), 256, 0, 0, &cpu) == NULL,
          "used this frame: not evicted");
    CHECK(rsx_vc_insert(c, key_of(2), 256, 4, 10, &cpu) == NULL,
          "old but submission 5 not complete: block not reused");
    CHECK(rsx_vc_find(c, key_of(1)) == NULL, "the old entry is gone from the table");
    CHECK(rsx_vc_insert(c, key_of(2), 256, 4, 10, &cpu) == NULL,
          "same frame: no rescan, still refused");
    CHECK(rsx_vc_insert(c, key_of(2), 256, 5, 11, &cpu) != NULL,
          "submission 5 complete: block reused");
    CHECK(g_ch.created == 1, "never more than the budget (%d chunks)", g_ch.created);
    rsx_vc_destroy(c);
}

/* Small blocks merge back into the chunk they were split from. */
static void test_coalesce(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    rsx_vertex_cache* c = rsx_vc_create(&k_chunks, CHUNK, CHUNK);
    u8* cpu;
    const u32 n = CHUNK / 256u;
    u32 ok = 0;
    for (u32 i = 0; i < n; i++) ok += rsx_vc_insert(c, key_of(100 + i), 200, 0, 0, &cpu) != NULL;
    CHECK(ok == n, "a chunk holds %u 256-byte blocks (%u)", n, ok);
    CHECK(rsx_vc_insert(c, key_of(7), 200, 0, 0, &cpu) == NULL, "and no more");
    /* All unused for 40 frames, GPU idle: the whole chunk comes back. */
    CHECK(rsx_vc_insert(c, key_of(8), CHUNK, 0, 40, &cpu) != NULL, "freed blocks coalesce");
    CHECK(g_ch.created == 1, "within one chunk");
    rsx_vc_destroy(c);
}

/* A budget under one chunk is kept: the chunks shrink to fit it. */
static void test_small_budget(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    rsx_vertex_cache* c = rsx_vc_create(&k_chunks, 100u << 10, 4u * CHUNK);
    u8* cpu;
    CHECK(c && rsx_vc_insert(c, key_of(1), 1000, 0, 0, &cpu) != NULL, "small budget: insert");
    CHECK(g_ch.created == 1 && g_ch.bytes == CHUNK,
          "one chunk of 64 KB for a 100 KB budget (%d of %u bytes)", g_ch.created, g_ch.bytes);
    CHECK(rsx_vc_insert(c, key_of(2), CHUNK + 1u, 0, 0, &cpu) == NULL,
          "a draw past the smaller chunk is refused");
    rsx_vc_destroy(c);
}

/* Random sizes, frames and GPU latency: live blocks never overlap and no
 * block is reused before its last reader completed. */
static void test_stress(void)
{
    memset(&g_ch, 0, sizeof g_ch);
    rsx_vertex_cache* c = rsx_vc_create(&k_chunks, 4u * CHUNK, CHUNK);
    enum { N = 4000 };
    static u32 live_key[N];
    u32 nlive = 0;
    /* The workload's own hits: the overlap check below looks entries up
     * too, and rsx_vc_find counts those in the cache's stats. */
    u32 hits = 0, lookups = 0;
    /* Per chunk granule: the serial of the newest submission that may read it. */
    static u64 reader[4][CHUNK / 256u];
    memset(reader, 0, sizeof reader);
    u32 seed = 12345;
    u64 serial = 1, completed = 0;
    int overlaps = 0, early = 0;
    for (u32 frame = 1; frame < 400; frame++) {
        for (u32 d = 0; d < 40; d++) {
            /* Four draws in five repeat one of a few hot keys (a frame's
             * steady geometry), the rest come from many more than fit. */
            seed = seed * 1103515245u + 12345u;
            const u32 r = seed >> 8;
            const u32 k = (r % 5u) ? r % 8u : 8u + (r >> 4) % 600u;
            rsx_vc_entry* e = rsx_vc_find(c, key_of(k));
            lookups++;
            if (e) hits++;
            if (!e) {
                seed = seed * 1103515245u + 12345u;
                const u32 bytes = 1u + (seed >> 8) % (CHUNK / 3u);
                u8* cpu;
                e = rsx_vc_insert(c, key_of(k), bytes, completed, frame, &cpu);
                if (!e) continue;
                const u32 ci = e->chunk - 1u, g0 = e->offset / 256u;
                const u32 gn = (u32)(((u64)1 << e->order));
                for (u32 g = g0; g < g0 + gn; g++)
                    if (reader[ci][g] > completed) early++;
                /* Overlap with any other live entry? */
                for (u32 i = 0; i < nlive; i++) {
                    rsx_vc_entry* o = rsx_vc_find(c, key_of(live_key[i]));
                    if (!o || o == e || o->chunk != e->chunk) continue;
                    const u32 oa = o->offset, ob = oa + (256u << o->order);
                    const u32 ea = e->offset, eb = ea + (256u << e->order);
                    if (oa < eb && ea < ob) overlaps++;
                }
                if (nlive < N) live_key[nlive++] = k;
            }
            rsx_vc_touch(e, serial, frame);
            const u32 ci = e->chunk - 1u, g0 = e->offset / 256u;
            for (u32 g = g0; g < g0 + (1u << e->order); g++) reader[ci][g] = serial;
        }
        serial++;                         /* one submission per frame       */
        if (serial > 3) completed = serial - 3;   /* GPU two frames behind */
    }
    CHECK(overlaps == 0, "live blocks never overlap (%d)", overlaps);
    CHECK(early == 0, "no block reused before its reader completed (%d)", early);
    rsx_vc_stats st;
    rsx_vc_get_stats(c, &st);
    CHECK(hits > lookups / 2 && st.evictions > 0,
          "the hot keys stay cached (%u hits of %u) while others are evicted (%llu)",
          hits, lookups, (unsigned long long)st.evictions);
    CHECK(g_ch.created <= 4, "budget kept (%d chunks)", g_ch.created);
    rsx_vc_destroy(c);
}

int main(void)
{
    test_hash();
    test_find_insert();
    test_deferred_reuse();
    test_coalesce();
    test_small_budget();
    test_stress();
    printf("vertex cache: %s (%d failures)\n", g_failures ? "FAIL" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
