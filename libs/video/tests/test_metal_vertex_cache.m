/* The Metal side of the draw engine's converted-vertex cache: a chunk is a
 * shared buffer the CPU writes, a cached draw records the chunk instead of
 * the staging arena, and every engine command buffer takes the serial
 * commit_serial announced for it and reports it completed once the GPU is
 * done -- what keeps a block from being reused while the GPU reads it.
 * Needs a Metal device; no window.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_vertex_cache.m \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o /tmp/test_metal_vertex_cache && /tmp/test_metal_vertex_cache
 */
#include "../rsx_metal_backend.m"
#include <stdio.h>
#include <unistd.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }

static int g_failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { printf("[FAIL] " __VA_ARGS__); printf("\n"); g_failures++; } \
    } while (0)

/* Completion handlers run on Metal's thread: wait for `serial` to land. */
static u64 wait_completed(u64 serial)
{
    u64 done = 0;
    for (int i = 0; i < 2000; i++) {
        eng_commit_serial(NULL, &done);
        if (done >= serial) break;
        usleep(1000);
    }
    return done;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device"); return 1; }
        s_queue = [s_dev newCommandQueue];
        if (!s_queue) { puts("no command queue"); return 1; }

        /* Nothing committed yet: the next submission is 1, none completed. */
        u64 done = ~0ull;
        const u64 first = eng_commit_serial(NULL, &done);
        CHECK(first == 1u && done == 0u, "fresh serials (next %llu, done %llu)",
              (unsigned long long)first, (unsigned long long)done);

        /* A chunk is a shared buffer whose contents the CPU writes. */
        u8* cpu = NULL;
        const u32 chunk = eng_vcache_chunk_create(NULL, 1u << 20, &cpu);
        CHECK(chunk && cpu && s_eng_vc_buf[chunk - 1u] &&
              cpu == (u8*)[s_eng_vc_buf[chunk - 1u] contents] &&
              [s_eng_vc_buf[chunk - 1u] storageMode] == MTLStorageModeShared,
              "a chunk is a shared buffer (%u)", chunk);

        /* A cached draw records the chunk and its offsets; a staged one
         * records none; an unknown chunk records nothing. */
        eng_draw_cached(NULL, RSX_TOPOLOGY_TRIANGLES, chunk, 256, 3, 64, 1024, 6);
        CHECK(s_eng_rec_count == 1, "the cached draw is recorded");
        const EngRecord* r = &s_eng_rec[0];
        CHECK(r->kind == ENG_REC_DRAW && r->vb_chunk == chunk && r->vb_off == 256 &&
              r->vertex_count == 3 && r->stride == 64 && r->ib_off == 1024 &&
              r->index_count == 6, "the record names the chunk and its offsets");
        const float v[3][4] = {{0}};
        eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 3, 16, NULL, 0);
        CHECK(s_eng_rec_count == 2 && s_eng_rec[1].vb_chunk == 0, "a staged draw names no chunk");
        eng_draw_cached(NULL, RSX_TOPOLOGY_TRIANGLES, chunk + 1u, 0, 3, 64, 0, 0);
        CHECK(s_eng_rec_count == 2, "an unknown chunk is not recorded");

        /* Not encoded: there are no targets or pipelines here. */
        s_eng_rec_count = 0;

        /* The commit takes the announced serial, and the GPU's completion
         * reports it; the next submission is announced past it. */
        eng_encode_and_commit(nil);
        CHECK(wait_completed(first) == first, "submission %llu completes",
              (unsigned long long)first);
        CHECK(eng_commit_serial(NULL, &done) == first + 1u, "the next one is %llu",
              (unsigned long long)first + 1ull);

        /* Serials only grow; completion follows the newest submission. */
        eng_encode_and_commit(nil);
        eng_encode_and_commit(nil);
        CHECK(wait_completed(first + 2u) == first + 2u, "completion follows the newest");
        CHECK(eng_commit_serial(NULL, &done) == first + 3u, "three submissions, three serials");

        eng_vcache_chunk_release(NULL, chunk);
        CHECK(!s_eng_vc_buf[chunk - 1u], "a released chunk is gone");
        eng_shutdown(NULL);
    }
    printf("metal vertex cache: %s (%d failures)\n", g_failures ? "FAIL" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
