/* The Metal backend's record cap (ENG_MAX_RECORDS): a full record array is
 * submitted mid-frame instead of dropping the rest of the frame, as a full
 * staging arena already was. The draw that finds it full opens the next
 * submission with the bound constants staged again in the next arena, and a
 * staged draw's vertices and indices share one reservation. Needs a Metal
 * device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_record_cap.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_record_cap && /tmp/test_metal_record_cap
 */
#include "../rsx_metal_backend.m"
#include <stdio.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

/* A title whose frames record more defines a larger record cap at build
 * time; the workflow builds this test a second time with one. */
#ifdef RSX_TEST_EXPECT_MAX_RECORDS
_Static_assert(ENG_MAX_RECORDS == RSX_TEST_EXPECT_MAX_RECORDS,
               "the record cap given at build time is the one the backend uses");
#endif

static int g_failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("[FAIL] " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device"); return 1; }
        s_queue = [s_dev newCommandQueue];
        if (!s_queue) { puts("no command queue"); return 1; }

        /* A staged draw's vertices and indices share one reservation; the
         * indices follow the vertices at the next aligned offset. */
        const float v[3][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}};
        const u32 idx[3] = {0, 1, 2};
        eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 3, 16, idx, 3);
        CHECK(s_eng_rec_count == 1, "the staged draw is recorded");
        const EngRecord* sd = &s_eng_rec[0];
        CHECK(sd->ib_off == sd->vb_off + ENG_ALIGN &&
              !memcmp(s_eng_stage + sd->vb_off, v, sizeof v) &&
              !memcmp(s_eng_stage + sd->ib_off, idx, sizeof idx),
              "a staged draw's indices follow its vertices (vb %u, ib %u)", sd->vb_off, sd->ib_off);
        /* Not encoded: no pipeline. */
        s_eng_rec_count = 0;
        s_eng_stage_used = 0;

        /* Fill the array with clears of one target (they encode as a single
         * pass), bind known constants, and record one more draw. */
        const u32 rt = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 16, 16, NULL, 0);
        CHECK(rt != 0, "a colour target");
        u8 vs_cb[64], ps_cb[32];
        for (u32 i = 0; i < sizeof vs_cb; i++) vs_cb[i] = (u8)(0xA0u + i);
        for (u32 i = 0; i < sizeof ps_cb; i++) ps_cb[i] = (u8)(0x10u + i);
        eng_bind_vs_constants(NULL, vs_cb, sizeof vs_cb);
        eng_bind_ps_constants(NULL, ps_cb, sizeof ps_cb);
        const float black[4] = {0, 0, 0, 1};
        while (s_eng_rec_count < ENG_MAX_RECORDS) eng_clear_color(NULL, rt, black);
        CHECK(s_eng_rec_count == ENG_MAX_RECORDS && !s_eng_dropped, "the array is full (%u)",
              s_eng_rec_count);

        /* The full array is submitted, not dropped: the draw that found it
         * full is the first record of the next submission. */
        s_eng_checked_completed = 0;
        eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 3, 16, idx, 3);
        CHECK(s_eng_checked_completed && !s_eng_encoding_failed,
              "the full array was submitted and completed without a failure");
        CHECK(s_eng_rec_count == 1 && !s_eng_dropped,
              "the draw opens the next submission (%u records, %u dropped)",
              s_eng_rec_count, s_eng_dropped);
        const EngRecord* d = &s_eng_rec[0];
        CHECK(d->vs_cb_bytes == sizeof vs_cb && d->ps_cb_bytes == sizeof ps_cb &&
              (u64)d->vs_cb_off + sizeof vs_cb <= s_eng_stage_used &&
              (u64)d->ps_cb_off + sizeof ps_cb <= s_eng_stage_used &&
              !memcmp(s_eng_stage + d->vs_cb_off, vs_cb, sizeof vs_cb) &&
              !memcmp(s_eng_stage + d->ps_cb_off, ps_cb, sizeof ps_cb),
              "its constants were staged again in the new arena");
        CHECK(!memcmp(s_eng_stage + d->vb_off, v, sizeof v) &&
              !memcmp(s_eng_stage + d->ib_off, idx, sizeof idx),
              "and its own vertices and indices are there");
        s_eng_rec_count = 0;
        s_eng_stage_used = 0;
        eng_shutdown(NULL);
    }
    printf("metal record cap: %s (%d failures)\n", g_failures ? "FAIL" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
