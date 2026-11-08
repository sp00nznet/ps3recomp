/* The draw engine's converted-vertex cache through a stub backend: a repeated
 * draw reuses the converted copy, and any change to what it was made from --
 * a vertex byte, an index, the vertex format -- converts again and draws the
 * new data. Every use is tagged with the submission it goes into. A draw the
 * key cannot describe (a per-instance attribute), PS3RECOMP_ENG_VCACHE=0 and
 * a backend without the hooks keep the staged path. Built like
 * test_color_feedback_engine.c. */
#define main engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"

#define M_IDXBUF_ADDR   0x181Cu
#define M_IDXBUF_FMT    0x1820u
#define M_IDX_BATCH     0x1824u

static rsx_draw_backend vc_backend;
static u8* vc_mem[8];
static u32 vc_chunks;
static u64 vc_committed, vc_done;   /* submissions committed / completed by the "GPU" */
static int n_cached;
static float cached_v0[4];       /* ATTR0 of the first vertex drawn */
static u32 cached_idx[8], cached_vcount, cached_icount;
static u32 cached_chunk, cached_off;

static u32 vc_create(void* u, u32 bytes, u8** cpu)
{
    (void)u;
    if (vc_chunks >= 8) return 0;
    vc_mem[vc_chunks] = (u8*)calloc(1, bytes);
    *cpu = vc_mem[vc_chunks];
    return ++vc_chunks;
}

static void vc_release(void* u, u32 chunk)
{
    (void)u;
    free(vc_mem[chunk - 1]);
    vc_mem[chunk - 1] = NULL;
}

static void vc_draw_cached(void* u, rsx_topology t, u32 chunk, u32 vb_off,
                           u32 vcount, u32 stride, u32 ib_off, u32 icount)
{
    (void)u; (void)t; (void)stride;
    n_cached++;
    cached_chunk = chunk;
    cached_off = vb_off;
    cached_vcount = vcount;
    cached_icount = icount;
    memcpy(cached_v0, vc_mem[chunk - 1] + vb_off, sizeof cached_v0);
    memset(cached_idx, 0xFF, sizeof cached_idx);
    if (icount)
        memcpy(cached_idx, vc_mem[chunk - 1] + ib_off,
               (icount < 8 ? icount : 8) * sizeof(u32));
}

static u64 vc_commit_serial(void* u, u64* completed)
{
    (void)u;
    *completed = vc_done;
    return vc_committed + 1u;        /* the submission being recorded */
}

/* What the backend's submit does: the draws so far go to the GPU, which
 * finishes them. */
static void commit(void) { vc_done = ++vc_committed; }

static void be_f32(u8* p, float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
}

static void set_vertex(u32 i, float x)
{
    u8* p = vm_base + VTX_OFFSET + i * 16u;
    be_f32(p, x); be_f32(p + 4, 0.5f); be_f32(p + 8, 0.0f); be_f32(p + 12, 1.0f);
}

static void up(int hooks)
{
    for (u32 i = 0; i < vc_chunks; i++) { free(vc_mem[i]); vc_mem[i] = NULL; }
    vc_chunks = 0;
    vc_committed = vc_done = 0;
    n_cached = 0;
    vc_backend = g_stub_backend;
    if (hooks) {
        vc_backend.vcache_chunk_create = vc_create;
        vc_backend.vcache_chunk_release = vc_release;
        vc_backend.draw_cached = vc_draw_cached;
        vc_backend.commit_serial = vc_commit_serial;
    }
    engine_up_with(&vc_backend);
}

static void draw_indexed(u32 count)
{
    m(M_IDXBUF_ADDR, IDX_OFFSET);
    m(M_IDXBUF_FMT, 1u | (1u << 4));  /* main memory, u16 */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_IDX_BATCH, 0u | ((count - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

static void set_index(u32 i, u16 v)
{
    u8* p = vm_base + IDX_OFFSET + i * 2u;
    p[0] = (u8)(v >> 8); p[1] = (u8)v;
}

/* The cache entry the last triangle draw used, found as sink_end finds it. */
static rsx_vc_entry* last_entry(void)
{
    rsx_vertex_layout_plan layout;
    eng_vertex_layout(&layout);
    rsx_vc_key ik, vk;
    int keyed, ranged;
    if (!eng_vc_index_key(RSX_PRIMITIVE_TRIANGLES, &layout, &ik)) return NULL;
    return eng_vc_lookup(ik, &layout, &vk, &keyed, &ranged);
}

static rsx_vc_stats stats(void)
{
    rsx_vc_stats st;
    rsx_vc_get_stats(g.vc, &st);
    return st;
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    for (u32 i = 0; i < 8; i++) set_vertex(i, (float)i);

    /* Arrays: the second identical draw reuses the first one's copy. */
    up(1);
    CHECK(g.vc != NULL, "the cache is on with the hooks");
    draw_triangle();
    const u32 chunk0 = cached_chunk, off0 = cached_off;
    CHECK(n_cached == 1 && stub.n_draw == 0, "a cacheable draw goes through draw_cached");
    CHECK(cached_v0[0] == 0.0f && cached_v0[1] == 0.5f && cached_v0[3] == 1.0f,
          "converted vertex 0 in the chunk (%g %g %g %g)",
          cached_v0[0], cached_v0[1], cached_v0[2], cached_v0[3]);
    draw_triangle();
    rsx_vc_stats st = stats();
    CHECK(n_cached == 2 && st.inserts == 1 && st.hits == 1,
          "the repeat is a hit (inserts %llu hits %llu)",
          (unsigned long long)st.inserts, (unsigned long long)st.hits);
    CHECK(cached_chunk == chunk0 && cached_off == off0, "the repeat draws the same block");

    /* A use is tagged with the submission the draw goes into, not with what
     * the GPU has finished: the block stays the GPU's until that is done. */
    rsx_vc_entry* e0 = last_entry();
    CHECK(e0 && e0->last_serial == vc_committed + 1u,
          "touched with the pending submission (%llu, pending %llu)",
          e0 ? (unsigned long long)e0->last_serial : 0ull,
          (unsigned long long)vc_committed + 1ull);
    commit();
    draw_triangle();
    CHECK(e0 && e0->last_serial == vc_committed + 1u && e0->last_serial == 2u,
          "a hit in the next submission moves it on (%llu)",
          e0 ? (unsigned long long)e0->last_serial : 0ull);

    /* One vertex byte changes: converted again, new data drawn. */
    set_vertex(0, 9.0f);
    draw_triangle();
    st = stats();
    CHECK(st.inserts == 2 && cached_v0[0] == 9.0f,
          "a changed vertex converts again (inserts %llu, x %g)",
          (unsigned long long)st.inserts, cached_v0[0]);
    draw_triangle();
    CHECK(stats().inserts == 2 && cached_v0[0] == 9.0f, "and that copy is reused");
    set_vertex(0, 0.0f);
    draw_triangle();
    CHECK(stats().inserts == 2 && cached_v0[0] == 0.0f && cached_off == off0,
          "the old bytes find the old copy again");

    /* The same vertices in another buffer (a title alternating two): the
     * address is not part of the key, the copy is shared. */
    memcpy(vm_base + VTX_OFFSET + 0x1000u, vm_base + VTX_OFFSET, 8u * 16u);
    m(M_VTXBUF_OFFSET, 0x80000000u | (VTX_OFFSET + 0x1000u));
    draw_triangle();
    CHECK(stats().inserts == 2 && cached_off == off0, "same bytes at another address: a hit");
    vm_base[VTX_OFFSET + 0x1000u] ^= 0x01u;
    draw_triangle();
    CHECK(stats().inserts == 3, "and a changed byte there is not");
    vm_base[VTX_OFFSET + 0x1000u] ^= 0x01u;
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);

    /* The current value of ATTR3 (colour) reaches the vertices only while
     * ATTR3 has no array: with one, changing it is still a hit. */
    rsx_vertex_layout_plan lp;
    eng_vertex_layout(&lp);
    if (lp.mask & (1u << 3)) {
        m(M_VTXFMT + 12u, 2u | (4u << 4) | (16u << 8));
        m(M_VTXBUF_OFFSET + 12u, 0x80000000u | VTX_OFFSET);
        draw_triangle();
        const u64 base = stats().inserts;
        m(0x1C30, 0x3F000000u);          /* current colour r = 0.5 */
        draw_triangle();
        CHECK(stats().inserts == base, "an array's attribute ignores its current value");
        m(M_VTXFMT + 12u, 2u);           /* ATTR3 array off */
        draw_triangle();
        const u64 off = stats().inserts;
        CHECK(off == base + 1, "turning the array off is another key");
        m(0x1C30, 0x3E800000u);          /* r = 0.25 */
        draw_triangle();
        CHECK(stats().inserts == off + 1, "without an array the current value is keyed");
    } else {
        CHECK(0, "the built-in program reads ATTR3 (mask %X)", lp.mask);
    }
    m(M_VTXFMT + 12u, 2u);
    const u64 before_fmt = stats().inserts;

    /* A different vertex format is a different key. */
    m(M_VTXFMT, 2u | (3u << 4) | (16u << 8));    /* float32 x3 */
    draw_triangle();
    CHECK(stats().inserts == before_fmt + 1 && cached_v0[3] == 1.0f, "a format change converts again");
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));

    /* An attribute fetched per instance (frequency 1 with its modulo bit:
     * element 0 for every vertex) reads outside the range the key covers:
     * such a draw is staged as before, never cached. */
    {
        const u64 ins = stats().inserts;
        const int cached = n_cached, staged = (int)stub.n_draw;
        m(0x1FC0, 1u);                                   /* ATTR0: modulo */
        m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (1u << 16));
        draw_triangle();
        draw_triangle();
        CHECK(n_cached == cached && (int)stub.n_draw == staged + 2 && stats().inserts == ins,
              "a per-instance attribute is staged (cached %d->%d, staged %d->%d)",
              cached, n_cached, staged, (int)stub.n_draw);
        m(0x1FC0, 0u);
        m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    }

    /* Indexed: indices are part of the key, and the rebuilt index list is
     * stored with the vertices. */
    set_index(0, 2); set_index(1, 1); set_index(2, 0);
    set_index(3, 2); set_index(4, 1); set_index(5, 0);
    draw_indexed(6);
    const u64 ins = stats().inserts;
    CHECK(n_cached > 0 && cached_icount == 6 && cached_vcount == 3,
          "indexed: 3 unique vertices, 6 indices (%u, %u)", cached_vcount, cached_icount);
    CHECK(cached_v0[0] == 2.0f, "first unique vertex is index 2 (%g)", cached_v0[0]);
    draw_indexed(6);
    CHECK(stats().inserts == ins && cached_icount == 6, "indexed repeat is a hit");
    set_index(5, 3);
    draw_indexed(6);
    CHECK(stats().inserts == ins + 1 && cached_vcount == 4,
          "a changed index converts again (%u unique)", cached_vcount);
    engine_down();

    /* PS3RECOMP_ENG_VCACHE=0: staged as before. */
    setenv("PS3RECOMP_ENG_VCACHE", "0", 1);
    up(1);
    draw_triangle();
    CHECK(g.vc == NULL && n_cached == 0 && stub.n_draw == 1, "the switch turns the cache off");
    engine_down();
    unsetenv("PS3RECOMP_ENG_VCACHE");

    /* A backend without the hooks: staged as before. */
    up(0);
    draw_triangle();
    CHECK(g.vc == NULL && n_cached == 0 && stub.n_draw == 1, "no hooks, no cache");
    engine_down();

    for (u32 i = 0; i < vc_chunks; i++) free(vc_mem[i]);
    free(vm_base);
    printf("engine vertex cache: %s (%d failures)\n", g_failures ? "FAIL" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
