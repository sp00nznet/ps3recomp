/* Authored CPU-only stream through the actual draw engine and recording backend.
 * An invalidation bug must change the decoded fields or emitted draw records.
 * The existing engine suite supplies content/alias/shader/reset regressions. */
#define main engine_regressions_main
#include "test_rsx_draw_engine.c"
#undef main
#include <time.h>
#include "rsx_test_programs.h"

extern void rsx_draw_engine_test_texture_decode_bypass(int);
extern u64 rsx_draw_engine_test_texture_decodes(void);
extern void rsx_draw_engine_test_get_texture(u32, rsx_dsp_texture*);

static void texture_decode_equivalence(void)
{
    static const u32 words[9] = {
        0x20000u, 0x0001A522u, 0x00030303u, 0x80000000u,
        0xAAE4u, 0x02020000u, 0x00040004u, 0xFFFFFFFFu, 0x00100010u
    };
    rsx_dispatch oracle;
    rsx_dispatch_init(&oracle, NULL);
    engine_up();
    rsx_draw_engine_test_texture_decode_bypass(0);
    draw_triangle();
    const u64 first = rsx_draw_engine_test_texture_decodes();
    draw_triangle();
    CHECK(first == 16 && rsx_draw_engine_test_texture_decodes() == first,
          "unchanged draws do not repeat texture register decoding");
    for (u32 unit = 0; unit < 16; unit++) {
        for (u32 w = 0; w < 9; w++) {
            const u32 method = w < 8 ? 0x1A00u + unit * 32u + w * 4u : 0x1840u + unit * 4u;
            /* First write a complete valid descriptor, then change every bit
             * independently at the same register addresses. Getter comparison
             * intentionally avoids interpreting malformed resources as images. */
            m(method, words[w]);
            rsx_dispatch_method(&oracle, method, words[w]);
        }
        for (u32 w = 0; w < 9; w++) for (u32 bit = 0; bit < 32; bit++) {
            const u32 method = w < 8 ? 0x1A00u + unit * 32u + w * 4u : 0x1840u + unit * 4u;
            const u32 value = words[w] ^ (1u << bit);
            /* Exercise normalization of the seven ordinary subchannel aliases. */
            m(method | ((bit % 7u) << 13), value);
            rsx_dispatch_method(&oracle, method, value);
            rsx_dsp_texture actual, expected;
            rsx_draw_engine_test_get_texture(unit, &actual);
            rsx_dsp_get_texture(&oracle, unit, &expected);
            if (memcmp(&actual, &expected, sizeof actual)) {
                CHECK(0, "unit %u word %u bit %u invalidates its decoded descriptor", unit, w, bit);
                engine_down(); return;
            }
            m(method, words[w]);
            rsx_dispatch_method(&oracle, method, words[w]);
            rsx_draw_engine_test_get_texture(unit, &actual);
            rsx_dsp_get_texture(&oracle, unit, &expected);
            if (memcmp(&actual, &expected, sizeof actual)) {
                CHECK(0, "restoring unit %u word %u bit %u invalidates", unit, w, bit);
                engine_down(); return;
            }
        }
    }
    CHECK(1, "all nine words, every bit, all units and subchannel aliases match scalar decode");
    const u64 before = rsx_draw_engine_test_texture_decodes();
    /* The descriptors are all populated. An identical write must preserve
     * reuse; changing unit 0 must leave unit 15 alone. */
    m(0x1A08, words[2]);
    rsx_dsp_texture t;
    rsx_draw_engine_test_get_texture(0, &t);
    CHECK(rsx_draw_engine_test_texture_decodes() == before,
          "identical register writes preserve decoded state");
    m(0x1A08, 4u);
    rsx_draw_engine_test_get_texture(15, &t);
    CHECK(rsx_draw_engine_test_texture_decodes() == before,
          "one unit's change does not decode another unit again");
    rsx_draw_engine_test_get_texture(0, &t);
    CHECK(t.wrap == 4u && rsx_draw_engine_test_texture_decodes() == before + 1,
          "changed unit is decoded exactly once");
    m(0xFA08, 9u); /* wider driver address, not unit 0's ordinary alias */
    rsx_draw_engine_test_get_texture(0, &t);
    CHECK(t.wrap == 4u && rsx_draw_engine_test_texture_decodes() == before + 1,
          "driver methods retain their wider address identity");
    engine_down();
    engine_up();
    for (u32 unit = 0; unit < 16; unit++) {
        rsx_dsp_texture actual;
        rsx_draw_engine_test_get_texture(unit, &actual);
        CHECK(!actual.enabled && !actual.width && !actual.offset,
              "restart discards unit %u's descriptor", unit);
    }
    engine_down();
}

/* Combined FIFO/decode-cache regression: exercise the actual bulk wrapper,
 * with the scalar register decoder as the oracle. No host graphics API. */
static void plain_be32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static int plain_texture_matches(u32 unit, const rsx_dispatch* oracle)
{
    rsx_dsp_texture actual, expected;
    rsx_draw_engine_test_get_texture(unit, &actual);
    rsx_dsp_get_texture(oracle, unit, &expected);
    return !memcmp(&actual, &expected, sizeof actual);
}

static void texture_decode_plain_packets(void)
{
    rsx_dispatch oracle;
    rsx_dispatch_init(&oracle, NULL);
    engine_up();
    rsx_draw_engine_test_texture_decode_bypass(0);
    u8 payload[16 * 4];
    for (u32 unit = 0; unit < 16; unit++)
        CHECK(plain_texture_matches(unit, &oracle), "prime plain-packet unit %u", unit);

    /* NI writes first the old value, then a changed value to each of the
     * nine dependency registers. Every unit must decode once, then reuse. */
    for (u32 unit = 0; unit < 16; unit++) for (u32 word = 0; word < 9; word++) {
        const u32 method = word < 8 ? 0x1A00u + unit * 32u + word * 4u : 0x1840u + unit * 4u;
        const u32 old = rsx_dsp_reg(&oracle, method), changed = old ^ (1u << (word + 1u));
        plain_be32(payload, old); plain_be32(payload + 4, changed);
        const u64 before = rsx_draw_engine_test_texture_decodes();
        CHECK(rsx_draw_engine_plain_run(method, payload, 2, 0), "admit NI texture dependency");
        rsx_dispatch_method(&oracle, method, old); rsx_dispatch_method(&oracle, method, changed);
        if (!plain_texture_matches(unit, &oracle) || rsx_draw_engine_test_texture_decodes() != before + 1) {
            CHECK(0, "NI dependency unit %u word %u invalidates exactly once", unit, word);
            engine_down(); return;
        }
        plain_be32(payload, changed);
        CHECK(rsx_draw_engine_plain_run(method, payload, 2, 0) &&
              plain_texture_matches(unit, &oracle) && rsx_draw_engine_test_texture_decodes() == before + 1,
              "same-value NI dependency preserves reuse");
    }
    CHECK(1, "all nine dependencies and all units invalidate under NI packets");

    for (u32 unit = 0; unit < 16; unit++) {
        const u32 method = 0x1A00u + unit * 32u;
        for (u32 word = 0; word < 8; word++) {
            const u32 value = rsx_dsp_reg(&oracle, method + word * 4u) ^ (0x101u + word);
            plain_be32(payload + word * 4u, value);
            rsx_dispatch_method(&oracle, method + word * 4u, value);
        }
        const u64 before = rsx_draw_engine_test_texture_decodes();
        CHECK(rsx_draw_engine_plain_run(method, payload, 8, 1) &&
              plain_texture_matches(unit, &oracle) && rsx_draw_engine_test_texture_decodes() == before + 1,
              "incrementing texture block invalidates its unit once");
        CHECK(rsx_draw_engine_plain_run(method, payload, 8, 1) &&
              plain_texture_matches(unit, &oracle) && rsx_draw_engine_test_texture_decodes() == before + 1,
              "same-value incrementing texture block preserves reuse");
    }
    for (u32 unit = 0; unit < 16; unit++) {
        const u32 method = 0x1840u + unit * 4u, value = rsx_dsp_reg(&oracle, method) ^ 1u;
        plain_be32(payload + unit * 4u, value); rsx_dispatch_method(&oracle, method, value);
    }
    const u64 size_before = rsx_draw_engine_test_texture_decodes();
    CHECK(rsx_draw_engine_plain_run(0x1840u, payload, 16, 1), "admit complete SIZE1 packet");
    for (u32 unit = 0; unit < 16; unit++) CHECK(plain_texture_matches(unit, &oracle), "bulk SIZE1 unit %u", unit);
    CHECK(rsx_draw_engine_test_texture_decodes() == size_before + 16, "SIZE1 packet invalidates all changed units");
    CHECK(rsx_draw_engine_plain_run(0x1840u, payload, 16, 1), "repeat complete SIZE1 packet");
    for (u32 unit = 0; unit < 16; unit++) CHECK(plain_texture_matches(unit, &oracle), "same SIZE1 unit %u", unit);
    CHECK(rsx_draw_engine_test_texture_decodes() == size_before + 16, "same SIZE1 packet retains all decoded units");

    /* Unsupported packets must not read even the first source byte or dirty
     * any valid unit. The boundary case starts on an eligible dependency. */
    const u8* unreadable = (const u8*)(uintptr_t)1;
    const u64 rejected_before = rsx_draw_engine_test_texture_decodes();
    CHECK(!rsx_draw_engine_plain_run(0x1BFCu, unreadable, 2, 1) &&
          !rsx_draw_engine_plain_run(0x183Cu, unreadable, 2, 1) &&
          !rsx_draw_engine_plain_run(0x1A01u, unreadable, 2, 1) &&
          !rsx_draw_engine_plain_run(0x1A00u, unreadable, 2048, 0) &&
          !rsx_draw_engine_plain_run(0x1A00u, unreadable, 0, 0) &&
          !rsx_draw_engine_plain_run(0x1A00u, NULL, 2, 1),
          "invalid packets reject before source access");
    for (u32 unit = 0; unit < 16; unit++) CHECK(plain_texture_matches(unit, &oracle), "rejected packets preserve unit %u", unit);
    CHECK(rsx_draw_engine_test_texture_decodes() == rejected_before, "rejected packets do not mutate cache validity");

    plain_be32(payload, rsx_dsp_reg(&oracle, 0x1BFCu) ^ 1u); plain_be32(payload + 4, 0x3F800000u);
    CHECK(!rsx_draw_engine_plain_run(0x1BFCu, payload, 2, 1), "mixed boundary packet takes scalar fallback");
    for (u32 i = 0; i < 2; i++) {
        const u32 method = 0x1BFCu + i * 4u;
        const u32 value = i ? 0x3F800000u : rsx_dsp_reg(&oracle, method) ^ 1u;
        m(method, value); rsx_dispatch_method(&oracle, method, value);
    }
    CHECK(plain_texture_matches(15, &oracle) && rsx_draw_engine_test_texture_decodes() == rejected_before + 1,
          "scalar fallback retains changed texture invalidation");
    engine_down();
    CHECK(!rsx_draw_engine_plain_run(0x1A00u, unreadable, 2, 0), "unready wrapper rejects before source access");
}

static u32 captured_ps[4];
static u8 captured_texture_byte;
static void capture_upload(void* user, u32 texture, u32 face, u32 mip, u32 width, u32 height,
                           const void* src, u32 row, u32 rows)
{
    if (src) captured_texture_byte = *(const u8*)src;
    stub_texture_upload(user, texture, face, mip, width, height, src, row, rows);
}
static void capture_ps(void* user, const void* bytes, u32 size)
{
    (void)user;
    if (bytes && size >= sizeof captured_ps) memcpy(captured_ps, bytes, sizeof captured_ps);
}

static void texture_decode_dynamic_inputs(void)
{
    rsx_draw_backend be = g_stub_backend;
    be.bind_ps_constants = capture_ps;
    be.texture_upload = capture_upload;
    engine_up();
    rsx_draw_engine_set_backend(&be);
    rsx_draw_engine_test_texture_decode_bypass(0);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    memset(vm_base + TEX_OFFSET, 0x11, 64);
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 word; memcpy(&word, vp + i * 4, 4); m(0xB80 + i * 4, word); }
    m(0x1EA0, 0);
    u8* fp = vm_base + 0x30000u;
    /* MOV r0, inline constant; END, then an authored float4 literal. */
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) |
                      RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      2u | RSX_TEST_FP_SWZ_IDENT);
    for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(fp + 16u + i * 4u, 0x3F000000u);
    m(0x08E4, 0x30000u | 2u);
    m(0x1D60, 0x40u);
    draw_triangle();
    CHECK(stub.n_draw == 1 && captured_ps[0] == 0x3F000000u,
          "authored guest program reaches backend with its inline constant");
    const int first_pipeline = stub.n_pipeline_create;
    const u64 decodes = rsx_draw_engine_test_texture_decodes();
    rsx_test_fp_put_word(fp + 16u, 0x3E800000u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == first_pipeline && captured_ps[0] == 0x3E800000u,
          "same-address fragment constant mutation reaches backend without a new pipeline");
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) |
                      RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      2u | RSX_TEST_FP_SWZ(2, 1, 0, 3));
    draw_triangle();
    CHECK(stub.n_pipeline_create == first_pipeline + 1,
          "same-address fragment instruction mutation selects a new pipeline");
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ(2, 1, 0, 3), 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 word; memcpy(&word, vp + i * 4, 4); m(0xB80 + i * 4, word); }
    draw_triangle();
    CHECK(stub.n_pipeline_create == first_pipeline + 2,
          "same-slot vertex program mutation selects a new pipeline");
    m(0x0A6C, 0x0203u);
    draw_triangle();
    CHECK(stub.pipeline_rs.depth_func == 0x0203u && stub.n_pipeline_create == first_pipeline + 3,
          "render-state changes still select and reach the correct pipeline");
    m(M_SURFACE_FORMAT, RSX_SURFACE_FMT_F_W16Z16Y16X16);
    draw_triangle();
    CHECK(stub.pipeline_rs.rt_fp16 == 1 && stub.n_pipeline_create == first_pipeline + 4,
          "surface format changes still reach the pipeline");
    CHECK(rsx_draw_engine_test_texture_decodes() == decodes,
          "shader, constant, render-state and surface changes leave pure texture descriptions reusable");
    m(0x08E4, 0); /* fixed program binds enabled units */
    draw_triangle();
    const u32 old_texture = stub.bound_tex[0];
    const int uploads = stub.n_texture_upload;
    rsx_draw_engine_present();
    memset(vm_base + TEX_OFFSET, 0x22, 64);
    draw_triangle();
    CHECK(stub.n_texture_upload > uploads && stub.bound_tex[0] != old_texture &&
          captured_texture_byte == 0x22,
          "same-address guest texture mutation is still observed with retained register decode");
    engine_down();
}

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void benchmark(unsigned draws, int changing, int bypass)
{
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    /* Warm all resource/pipeline allocations before measuring. */
    rsx_draw_engine_test_texture_decode_bypass(bypass);
    draw_triangle();
    rsx_draw_engine_test_texture_decode_bypass(bypass);
    const double start = seconds();
    for (unsigned i = 0; i < draws; i++) {
        /* Changing-state control: each unit's sampler word changes. */
        if (changing) for (u32 unit = 0; unit < 16; unit++)
            m(0x1A08u + unit * 32u, 1u + (i & 1u));
        draw_triangle();
    }
    const double elapsed = seconds() - start;
    printf("BENCH,%s,%s,%u,%.9f,%llu,%d\n", changing ? "changing" : "repeated",
           bypass == 1 ? "scalar" : bypass == 2 ? "per_draw" : "reuse", draws, elapsed,
           (unsigned long long)rsx_draw_engine_test_texture_decodes(), stub.n_draw);
    CHECK(stub.n_draw == (int)draws + 1, "every benchmark draw reaches backend");
    engine_down();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The existing suite initializes authored guest vertex data and executes
     * content, aliases, programs/constants, render state and lifecycle tests. */
    if (argc == 1) {
        int result = engine_regressions_main();
        if (result) return result;
    }
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    if (!vm_base) return 1;
    for (u32 i = 0; i < 12; i++) vm_base[VTX_OFFSET + i * 4u] = 0x3F;
    if (argc > 1) {
        const unsigned draws = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 100000;
        for (int repeat = 0; repeat < 9; repeat++) for (int changing = 0; changing < 2; changing++) {
            /* Alternate order to reduce temperature/drift bias. */
            for (int mode = 0; mode < 3; mode++)
                benchmark(draws, changing, (mode + repeat) % 3);
        }
    } else { texture_decode_equivalence(); texture_decode_plain_packets(); texture_decode_dynamic_inputs(); }
    free(vm_base);
    return g_failures ? 1 : 0;
}
