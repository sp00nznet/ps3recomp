/* Synthetic draw-state and source-contract checks; no guest title data. */
#define main existing_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
static char captured_ps[65536];
static u32 capture_pipeline(void* u, const char* vs, const char* ps,
 const rsx_be_render_state* rs, const rsx_vertex_layout_plan* l,
 u32 stride, rsx_be_format f, u32 n)
{
    snprintf(captured_ps, sizeof captured_ps, "%s", ps);
    return stub_pipeline_create(u, vs, ps, rs, l, stride, f, n);
}
static void load_program(void)
{
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 8; ++i) { u32 x; memcpy(&x, vp + i * 4, 4); m(0xB80 + i * 4, x); }
    m(0x1EA0, 0);
    rsx_test_fp_instr(vm_base + 0x60000,
        RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 |
        RSX_TEST_FP_END | (1u << 7), RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, 0x60002); m(0x1D60, 0);
}
int main(int argc, char** argv)
{
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    engine_up(); rsx_draw_backend be = g_stub_backend;
    be.pipeline_create = capture_pipeline; rsx_draw_engine_set_backend(&be);
    load_program(); m(M_SURFACE_FORMAT, 0x148); m(0x1FEC, 0); draw_triangle();
    const u32 raw = stub.bound_pipeline; char raw_ps[65536];
    memcpy(raw_ps, captured_ps, sizeof raw_ps);
    CHECK(stub.n_pipeline_create == 1, "disabled draw compiles once");
    if (argc == 2) { char path[1024]; snprintf(path, sizeof path, "%s/raw-packer.ps.hlsl", argv[1]); FILE* f = fopen(path, "wb"); if (f) { fputs(captured_ps, f); fclose(f); } }
    m(0x1FEC, 1); draw_triangle(); const u32 encoded = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 2 && encoded != raw, "enabled packer changes pipeline");
    CHECK(strstr(captured_ps, "_rsx_srgb_output") != NULL, "enabled draw has original RGB conversion");
    CHECK(stub.bound_mask == 0, "output helper adds no sampler binding");
    if (argc == 2) { char path[1024]; snprintf(path, sizeof path, "%s/active-packer.ps.hlsl", argv[1]); FILE* f = fopen(path, "wb"); if (f) { fputs(captured_ps, f); fclose(f); } }
    m(0x1FEC, 0x80000000u); draw_triangle();
    CHECK(stub.bound_pipeline == encoded && stub.n_pipeline_create == 2, "any nonzero packer shares enabled state");
    m(0x1FEC, 0); draw_triangle();
    CHECK(stub.bound_pipeline == raw && stub.n_pipeline_create == 2, "disabled switchback reuses raw shader");
    m(0x1D60, 0x40); m(0x1FEC, 0); draw_triangle(); const u32 full = stub.bound_pipeline;
    m(0x1FEC, 1); draw_triangle();
    CHECK(stub.bound_pipeline == full && !strstr(captured_ps, "_rsx_srgb_output"), "32-bit exports ignore packer");
    m(0x1D60, 0);
    for (u32 format = 0xB; format <= 0xD; ++format) {
        m(M_SURFACE_FORMAT, 0x140 | format); m(0x1FEC, 0); draw_triangle();
        const u32 floating = stub.bound_pipeline; const int count = stub.n_pipeline_create;
        m(0x1FEC, 1); draw_triangle();
        CHECK(stub.bound_pipeline == floating && count == stub.n_pipeline_create &&
              !strstr(captured_ps, "_rsx_srgb_output"), "float target bypasses conversion and key");
    }
    m(M_SURFACE_FORMAT, 0x148); m(0x1FEC, 1); draw_triangle();
    CHECK(stub.bound_pipeline == encoded, "integer format switchback retains encoded variant");
    for (u32 format = 1; format <= 0x10; ++format) {
        const int integer = format != 0xB && format != 0xC && format != 0xD;
        CHECK(rsx_fp_srgb_output_enabled(0x10, 0, format) == integer, "surface eligibility matches original formats");
        CHECK(!rsx_fp_srgb_output_enabled(0, 0, format), "disabled state always bypasses");
        CHECK(!rsx_fp_srgb_output_enabled(1, 0x40, format), "32-bit exports always bypass");
    }
    u8 fp[64]; char hlsl[65536], before[65536];
    const u32 regs[4] = {0, 4, 6, 8};
    for (u32 t = 0; t < 4; ++t) rsx_test_fp_instr(fp + t * 16,
        RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 |
        RSX_TEST_FP_OUTREG(regs[t]) | (1u << 7) | (t == 3 ? RSX_TEST_FP_END : 0),
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(t%4, (t+1)%4, (t+2)%4, 3));
    CHECK(rsx_fp_decompile_ex(fp, sizeof fp, 0, 0, hlsl, sizeof hlsl) > 0, "four half-export MRT program decompiles");
    memcpy(before, hlsl, sizeof hlsl);
    CHECK(rsx_fp_apply_srgb_output(hlsl, (u32)strlen(hlsl) + 1) == -1 && !memcmp(before, hlsl, sizeof hlsl), "capacity failure is atomic");
    CHECK(rsx_fp_apply_srgb_output(hlsl, sizeof hlsl) == 1, "all MRT exports accept conversion");
    for (u32 t = 0; t < 4; ++t) { char fragment[96]; snprintf(fragment, sizeof fragment, "_rsx_color_out.t%u = _rsx_srgb_output(_rsx_color_out.t%u)", t, t); CHECK(strstr(hlsl, fragment), "MRT RGB output is converted separately"); }
    if (argc == 2) { char path[1024]; snprintf(path, sizeof path, "%s/mrt-packer.ps.hlsl", argv[1]); FILE* f = fopen(path, "wb"); if (f) { fputs(hlsl, f); fclose(f); } }
    CHECK(rsx_fp_apply_alpha_test_buffered(hlsl, sizeof hlsl, 0x204) == 1, "alpha test composes after RGB conversion");
    CHECK(strstr(hlsl, "_rsx_out.t0.a"), "alpha test uses unchanged alpha");
    memcpy(before, hlsl, sizeof hlsl);
    CHECK(rsx_fp_apply_srgb_output(hlsl, sizeof hlsl) == -1 && !memcmp(before, hlsl, sizeof hlsl), "duplicate conversion fails without mutation");
    char malformed[64] = "float4 main() { return 0; }"; char old[64]; memcpy(old, malformed, sizeof old);
    CHECK(rsx_fp_apply_srgb_output(malformed, sizeof malformed) == -1 && !memcmp(old, malformed, sizeof old), "malformed source is atomic");
    CHECK(rsx_fp_apply_srgb_output(NULL, 0) == -1, "null source rejected");
    engine_down(); free(vm_base);
    printf("framebuffer sRGB: %d failures\n", g_failures); return g_failures ? 1 : 0;
}
