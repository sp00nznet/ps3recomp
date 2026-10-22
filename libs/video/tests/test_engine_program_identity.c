/* Synthetic draw-engine test of pipeline identity across many guest program
 * pairs: each distinct transform program gets its own pipeline, revisiting
 * any of them reuses it, and a transform program rewritten in place is a new
 * pipeline. Also the inline-constant collector's slot contract. No title
 * data.
 *
 * Build like test_rsx_draw_engine.c, substituting this file and adding
 * -I libs/video/tests. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"

#define N_PROGRAMS 256u

static void upload_vp(u32 swz)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, swz, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static u32 swizzle(u32 i)
{
    return RSX_TEST_VP_SWZ(i & 3u, (i >> 2) & 3u, (i >> 4) & 3u, (i >> 6) & 3u);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);

    u32 handle[N_PROGRAMS];
    for (u32 i = 0; i < N_PROGRAMS; i++) {
        upload_vp(swizzle(i));
        draw_triangle();
        handle[i] = stub.bound_pipeline;
    }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS,
          "each distinct transform program builds one pipeline (%d)", stub.n_pipeline_create);
    int distinct = 1;
    for (u32 i = 0; i < N_PROGRAMS; i++)
        for (u32 j = i + 1; j < N_PROGRAMS; j++)
            if (handle[i] == handle[j]) distinct = 0;
    CHECK(distinct, "and the pipelines are distinct");

    int same = 1;
    for (u32 r = 0; r < 2; r++)
        for (u32 k = 0; k < N_PROGRAMS; k++) {
            const u32 i = (k * 97u + r) % N_PROGRAMS;
            upload_vp(swizzle(i));
            draw_triangle();
            if (stub.bound_pipeline != handle[i]) same = 0;
        }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS && same,
          "revisiting every program reuses its own pipeline (%d)", stub.n_pipeline_create);

    /* The same slot, the same length, different bytes: a new program. */
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.bound_pipeline == handle[5], "program 5 is bound");
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 1, swizzle(5), 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline != handle[5],
          "a program rewritten in place is not taken for the old one (%d)", stub.n_pipeline_create);
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline == handle[5],
          "and restoring it finds the old pipeline again");
    engine_down();

    /* Only the slots a program has are written. */
    static rsx_fp_constant_block block;
    memset(&block, 0xAB, sizeof block);
    u8 fp[64] = {0};
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    CHECK(rsx_fp_collect_constants(fp, sizeof fp, &block) == 0 && block.count == 0,
          "a program without constants has no slots");
    CHECK(block.values[0][0] == 0xABABABABu, "and the unused slots are left alone");

    free(vm_base);
    printf("program identity failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
