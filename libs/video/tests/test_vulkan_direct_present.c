/* Vulkan engine presents: a direct present (PS3RECOMP_VK_DIRECT_PRESENT=1)
 * must leave the same frame a read-back present (the default) does, keep
 * the counters honest, and say how much CPU a present call costs.
 *
 * Each frame clears a target, draws blended full-screen quads into it and
 * presents it through the engine's present callback; the fixture times the
 * present call alone. One mode per process: run it twice, with and without
 * PS3RECOMP_VK_DIRECT_PRESENT=1, and compare the two OUT files byte for
 * byte. Headless unless PS3RECOMP_VK_WINDOW=1 is set. Run it under the
 * Khronos validation layer to check the direct path's synchronisation.
 * Synthetic: no guest memory, no game data.
 *
 *   test_vulkan_direct_present LABEL OUT [FRAMES [DRAWS_PER_FRAME [FRAME_MS]]] */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_vulkan_backend.c"
#include <assert.h>
#include <time.h>

/* The pieces of the runtime the backend and the engine reach for, none of
 * which these frames use: no guest memory, no FIFO, no display buffers. */
u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; (void)offset; assert(!"no guest memory"); return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; assert(!"no guest IO"); return 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
static rsx_backend* s_fixture_backend;
void rsx_set_backend(rsx_backend* backend) { s_fixture_backend = backend; }
rsx_backend* rsx_get_backend(void) { return s_fixture_backend; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

static const char* VS =
    "struct VSIn { float4 a0 : ATTR0; float4 a1 : ATTR1; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.a0.xy, 0.5, 1.0); o.col = i.a1; return o; }\n";
static const char* PS =
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; };\n"
    "float4 main(VSOut i) : SV_Target0 { return i.col; }\n";

enum { W = 640, H = 360 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp_double(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 6) {
        fprintf(stderr, "usage: %s LABEL OUT [FRAMES [DRAWS_PER_FRAME [FRAME_MS]]]\n", argv[0]);
        return 2;
    }
    const u32 frames = argc > 3 ? (u32)strtoul(argv[3], NULL, 0) : 24;
    const u32 draws  = argc > 4 ? (u32)strtoul(argv[4], NULL, 0) : 8;
    /* Optional pacing, so a window's vsync does not set the measured cost. */
    const double frame_ms = argc > 5 ? atof(argv[5]) : 0.0;
    if (!frames) return 2;
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    if (!getenv("PS3RECOMP_VK_WINDOW")) setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    if (rsx_vulkan_backend_init(W, H, "direct present synthetic") != 0) {
        printf("SKIP: no Vulkan device\n");
        return 0;
    }
    CHECK(s_vk.eng_active, "the register-file engine drives the backend");
    CHECK(s_g.on, "guest programs (glslang) available");
    if (!s_vk.eng_active || !s_g.on) { rsx_vulkan_backend_shutdown(); return 1; }
    const int direct = vk_env_on("PS3RECOMP_VK_DIRECT_PRESENT");

    const u32 rt = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, W, H, NULL, 0);
    rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
    rs.color_mask = 0xFFFFFFFFu;
    rs.blend_enable = 1; rs.sf_rgb = rs.sf_a = 0x0302; rs.df_rgb = rs.df_a = 0x0303;
    rs.eq_rgb = rs.eq_a = 0x8006;   /* src alpha, one minus src alpha, add */
    rsx_vertex_layout_plan lp; memset(&lp, 0, sizeof lp);
    lp.mask = 3; lp.stride = 32; lp.count = 2;
    const u32 pipe = vk_eng_pipeline_create(NULL, VS, PS, &rs, &lp, 32, RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(rt && pipe, "target and pipeline");
    if (!rt || !pipe) { rsx_vulkan_backend_shutdown(); return 1; }

    double* cost = calloc(frames, sizeof *cost);
    static float v[6 * 8];
    static const float p[6][2] = { {-1,-1},{1,-1},{-1,1},{1,-1},{1,1},{-1,1} };
    const double t0 = now_ms();
    for (u32 f = 0; f < frames; f++) {
        const double frame_start = now_ms();
        const float clear[4] = { (float)(f % 3) / 2.0f, 0.25f, 0.5f, 1.0f };
        vk_eng_clear_color(NULL, rt, clear);
        vk_eng_bind_targets(NULL, &rt, 1, 0);
        vk_eng_bind_pipeline(NULL, pipe);
        vk_eng_set_viewport(NULL, 0, 0, W, H);
        vk_eng_set_scissor(NULL, 0, 0, W, H);
        for (u32 d = 0; d < draws; d++) {
            const float c[4] = { (float)((f + d) % 7) / 6.0f, (float)(d % 5) / 4.0f, 0.125f, 0.25f };
            for (int i = 0; i < 6; i++) {
                v[i * 8] = p[i][0]; v[i * 8 + 1] = p[i][1]; v[i * 8 + 2] = 0; v[i * 8 + 3] = 1;
                memcpy(v + i * 8 + 4, c, 16);
            }
            vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 6, 32, NULL, 0);
        }
        const double s = now_ms();
        vk_eng_present(NULL, rt);
        cost[f] = now_ms() - s;
        while (now_ms() - frame_start < frame_ms) {
            struct timespec ts = { 0, 200000 };
            nanosleep(&ts, NULL);
        }
    }
    vk_eng_submit_and_wait(NULL, 0);
    const double total = now_ms() - t0;

    static u8 out[W * H * 4];
    vk_eng_readback(NULL, rt, 0, 0, W, H, out, W * 4);
    const u8* c = out + ((size_t)(H / 2) * W + W / 2) * 4;
    const u32 centre = ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
    CHECK(s_vk.presented == frames, "every present counted");
    if (direct) {
        CHECK(s_vk.present_direct == frames && s_vk.present_readback_count == 0, "every present direct");
        CHECK(rsx_vulkan_backend_presented_center() == 0, "a direct present keeps no centre pixel");
    } else {
        CHECK(s_vk.present_readback_count == frames && s_vk.present_direct == 0, "every present read back");
        CHECK((rsx_vulkan_backend_presented_center() & 0xFFFFFFu) == centre,
              "a read-back present kept the centre pixel");
    }
    /* Every slot of the ring is idle once the GPU has finished. */
    vk_device_idle();
    vk_present_wait_gpu();

    FILE* fo = fopen(argv[2], "wb");
    CHECK(fo && fwrite(out, 1, sizeof out, fo) == sizeof out, "frame written");
    if (fo) fclose(fo);

    qsort(cost, frames, sizeof *cost, cmp_double);
    double sum = 0; for (u32 f = 0; f < frames; f++) sum += cost[f];
    printf("%s (%s): %u frames x %u draws at %ux%u, present call median %.1f us, p90 %.1f us, mean %.1f us; "
           "loop %.2f ms/frame\n", argv[1], direct ? "direct" : "read-back", frames, draws, W, H,
           cost[frames / 2] * 1e3, cost[frames * 9 / 10] * 1e3, sum / frames * 1e3, total / frames);
    free(cost);
    vk_eng_release(NULL, rt);
    vk_eng_pipeline_release(NULL, pipe);
    rsx_vulkan_backend_shutdown();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
