/* The GPU-side depth-as-A8R8G8B8 pass (rsx_vulkan_depth_argb.inc) against the
 * CPU path it replaces: depth readback, the engine's guest store
 * (rsx_depth_guest_z24) and rsx_texture_decode.
 *   1. Every float bit pattern from +0 to 1.0, plus negatives, NaN, infinities
 *      and values above 1, fed straight into the expanded depth texture.
 *   2. Real 2x and 4x depth targets drawn with interpolated depths, at raster
 *      scale 1 and 2, compared with what vk_eng_depth_readback returns.
 * Synthetic data only; no game/firmware bytes. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include "../libs/video/rsx_texture_layout.h"
#include <math.h>
uint8_t* vm_base;
static unsigned long long checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

/* The CPU path's host texels for `n` depth samples, through the real decoder. */
static void cpu_texels(const float* depth, u32 w, u32 h, u32 rsx_fmt, int rgba, u8* guest, u8* host)
{
    for (u32 i = 0; i < w * h; ++i) {
        const u32 v = rsx_depth_guest_z24(depth[i]);
        guest[i*4] = (u8)(v >> 16); guest[i*4+1] = (u8)(v >> 8); guest[i*4+2] = (u8)v; guest[i*4+3] = 0;
    }
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    rsx_texture_mip_chain(rsx_fmt, w, h, 1, w * 4u, lv);
    rsx_texture_decode(host, w * 4u, guest, w, h, &lv[0].tl, rgba);
}

/* Feed `n` floats (w x h) as the expanded samples of a 2x depth target and
 * compare the converted texture with the CPU path. Returns mismatching texels. */
static u32 run_values(u32 depth, u32 w, u32 h, const float* values, u32 rsx_fmt, u32 flags,
                      u8* guest, u8* want, u8* got)
{
    const u32 expanded = vk_eng_expand_samples(depth);
    vk_eng_texture_upload(NULL, expanded, 0, 0, w, h, values, w * 4u, h);
    const u32 tex = vk_eng_depth_to_argb(NULL, depth, w, h, 0xAAE4u, rsx_fmt, flags);
    if (!tex) return UINT32_MAX;
    vk_eng_readback(NULL, tex, 0, 0, w, h, got, w * 4u);
    cpu_texels(values, w, h, rsx_fmt, (int)(flags & 1u), guest, want);
    u32 bad = 0;
    for (u32 i = 0; i < w * h; ++i)
        if (memcmp(want + i * 4u, got + i * 4u, 4)) {
            if (!bad) fprintf(stderr, "first mismatch: depth %a (0x%08X) want %02X%02X%02X%02X got %02X%02X%02X%02X\n",
                              (double)values[i], *(const u32*)&values[i], want[i*4], want[i*4+1], want[i*4+2], want[i*4+3],
                              got[i*4], got[i*4+1], got[i*4+2], got[i*4+3]);
            ++bad;
        }
    checks += w * h;
    return bad;
}

int main(void)
{
    SDL_SetMainReady();
    CHECK(rsx_vulkan_backend_init(64, 64, "depth-as-ARGB regression") == 0);
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT && s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT)
        printf("note: depth format %d\n", (int)s_vk.depth_format);

    /* 1. Every non-negative float up to 1.0, then the special values. */
    {
        const u32 W = 4096, H = 2048, N = W * H;
        const u32 depth = vk_eng_depth_multisample(NULL, W / 2u, H, 2);
        CHECK(depth != 0);
        float* values = (float*)malloc((size_t)N * 4u);
        u8* guest = (u8*)malloc((size_t)N * 4u);
        u8* want = (u8*)malloc((size_t)N * 4u);
        u8* got = (u8*)malloc((size_t)N * 4u);
        CHECK(values && guest && want && got);
        const u32 last = 0x3F800000u;                   /* 1.0f */
        u32 bits = 0, chunks = 0;
        while (bits <= last) {
            for (u32 i = 0; i < N; ++i) {
                const u32 b = bits + i <= last ? bits + i : last;
                memcpy(&values[i], &b, 4);
            }
            CHECK(run_values(depth, W, H, values, 0x85u | 0x20u, 0, guest, want, got) == 0);
            ++chunks;
            if (last - bits < N) break;
            bits += N;
        }
        static const u32 specials[] = { 0x80000000u, 0xBF800000u, 0xFF800000u, 0x7F800000u, 0x7FC00000u,
                                        0xFFC00000u, 0x7F800001u, 0x3F800001u, 0x40000000u, 0x00000001u,
                                        0x007FFFFFu, 0x80000001u, 0x3F7FFFFFu, 0x3F7FFFFEu, 0x34000000u };
        for (u32 i = 0; i < N; ++i) memcpy(&values[i], &specials[i % (sizeof specials / 4u)], 4);
        CHECK(run_values(depth, W, H, values, 0x85u | 0x20u, 0, guest, want, got) == 0);
        /* The other output orders on a spread of values: RGBA decode and D8R8G8B8. */
        for (u32 i = 0; i < N; ++i) { const u32 b = (u32)(((unsigned long long)i * 127u) % (last + 1u)); memcpy(&values[i], &b, 4); }
        CHECK(run_values(depth, W, H, values, 0x9Eu | 0x20u, 0, guest, want, got) == 0);
        CHECK(run_values(depth, W, H, values, 0x85u | 0x20u, 1, guest, want, got) == 0);
        CHECK(run_values(depth, W, H, values, 0x9Eu | 0x20u | 0x40u, 1, guest, want, got) == 0);
        /* A smaller texture than the target reads its top-left corner. */
        CHECK(run_values(depth, 640, 360, values, 0x85u | 0x20u, 0, guest, want, got) == 0);
        printf("exhaustive: %u chunks, every float in [0, 1] plus specials and both orders\n", chunks);
        /* Release takes the converted texture with it. */
        const u32 tex = s_eobj[depth].argb;
        CHECK(tex && vk_eng_get(tex, VK_ENG_TEXTURE));
        vk_eng_release(NULL, depth);
        CHECK(!vk_eng_get(tex, VK_ENG_TEXTURE));
        free(values); free(guest); free(want); free(got);
    }

    /* 2. Real draws: a full-target triangle pair whose depth runs 0..1 across
     * x, then a second write; 2x and 4x; raster scale 1 and 2. */
    static const char vs[] = "float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char fs[] = "float4 main():SV_Target0{return float4(1,0,0,1);}";
    rsx_vertex_layout_plan layout = {0}; layout.count = 1; layout.stride = 16; layout.attrs[0] = 0;
    rsx_be_render_state state = {0}; state.color_mask = 0x01010101u;
    state.depth_test = 1; state.depth_write = 1; state.depth_func = 0x207;   /* ALWAYS */
    for (u32 scale = 1; scale <= 2; ++scale) {
        s_render_scale = scale;
        for (u32 samples = 2; samples <= 4; samples += 2) {
            state.antialias_control = 0xFFFF0001u;
            const u32 pipeline = vk_eng_pipeline_create(NULL, vs, fs, &state, &layout, 16, RSX_BE_FMT_R8G8B8A8, 1);
            CHECK(pipeline != 0);
            const u32 LW = 160, LH = 90;
            const u32 color = vk_eng_color_multisample(NULL, RSX_BE_FMT_R8G8B8A8, LW, LH, samples);
            const u32 depth = vk_eng_depth_multisample(NULL, LW, LH, samples);
            CHECK(color && depth);
            const u32 W = LW * 2u, H = LH * (samples == 4 ? 2u : 1u);
            float* floats = (float*)malloc((size_t)W * H * 4u);
            u8* guest = (u8*)malloc((size_t)W * H * 4u);
            u8* want = (u8*)malloc((size_t)W * H * 4u);
            u8* got = (u8*)malloc((size_t)W * H * 4u);
            CHECK(floats && guest && want && got);
            for (u32 pass = 0; pass < 2; ++pass) {
                const float z0 = pass ? 0.9f : 0.0f, z1 = pass ? 0.1f : 1.0f;
                const float v[6][4] = { {-1,-1,z0,1}, {1,-1,z1,1}, {1,1,z1,1}, {-1,-1,z0,1}, {1,1,z1,1}, {-1,1,z0,1} };
                vk_eng_bind_targets(NULL, &color, 1, depth); vk_eng_bind_pipeline(NULL, pipeline);
                vk_eng_set_viewport(NULL, 0, 0, (float)LW, (float)LH, 0, 1); vk_eng_set_scissor(NULL, 0, 0, LW, LH);
                vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 6, 16, NULL, 0);
                const u32 tex = vk_eng_depth_to_argb(NULL, depth, W, H, 0xAAE4u, 0x85u | 0x20u, 0);
                CHECK(tex != 0);
                CHECK(vk_eng_depth_readback(NULL, depth, 0, 0, W, H, floats, W * 4u) == 0);
                vk_eng_readback(NULL, tex, 0, 0, W, H, got, W * 4u);
                cpu_texels(floats, W, H, 0x85u | 0x20u, 0, guest, want);
                u32 distinct = 0, bad = 0;
                for (u32 i = 0; i < W * H; ++i) {
                    if (memcmp(want + i * 4u, got + i * 4u, 4)) ++bad;
                    if (i && floats[i] != floats[i - 1]) ++distinct;
                }
                checks += W * H;
                if (bad) fprintf(stderr, "scale %u samples %u pass %u: %u of %u texels differ\n", scale, samples, pass, bad, W * H);
                CHECK(bad == 0);
                CHECK(distinct > W / 2u);                   /* the depth really varies */
            }
            free(floats); free(guest); free(want); free(got);
            vk_eng_release(NULL, depth); vk_eng_release(NULL, color); vk_eng_pipeline_release(NULL, pipeline);
        }
    }
    s_render_scale = 1;
    /* Single-sample depth stays on the CPU path. */
    {
        const u32 depth = vk_eng_depth_target_create(NULL, 8, 4);
        CHECK(depth && vk_eng_depth_to_argb(NULL, depth, 8, 4, 0xAAE4u, 0x85u | 0x20u, 0) == 0);
        vk_eng_release(NULL, depth);
    }
    vk_eng_shutdown(NULL);
    printf("depth-as-ARGB on the GPU: %llu checks passed\n", checks);
    return 0;
}
