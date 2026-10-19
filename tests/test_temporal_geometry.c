#include "../libs/video/rsx_temporal_geometry.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static const rsx_tg_clip tri[3] = {
    {-0.5f, -0.5f, 0.5f, 1.0f},
    { 0.5f, -0.5f, 0.5f, 1.0f},
    { 0.0f,  0.5f, 0.5f, 1.0f}
};

static void near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.0002f);
}

static void test_motion(void)
{
    const float bary[3] = {0.25f, 0.25f, 0.5f};
    rsx_tg_clip now[3], old[3];
    rsx_tg_motion motion = {123.0f, 456.0f};
    unsigned i;

    for (i = 0; i < 3; ++i) {
        now[i] = tri[i]; old[i] = tri[i];
        now[i].x += 0.02f; old[i].x -= 0.04f;
        now[i].y -= 0.04f; old[i].y += 0.02f;
    }
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  1, 2, -2, -1, &motion) == RSX_TG_OK);
    near(motion.x, 0); near(motion.y, 0); /* Jitter alone cancels. */

    for (i = 0; i < 3; ++i) {
        now[i] = tri[i]; old[i] = tri[i];
        old[i].x += 0.2f;
        old[i].y += 0.2f;
    }
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 80,
                                  0, 0, 0, 0, &motion) == RSX_TG_OK);
    near(motion.x, 10); near(motion.y, -8); /* Screen y is down. */
    for (i = 0; i < 3; ++i) old[i].x = now[i].x - 0.2f;
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 80,
                                  0, 0, 0, 0, &motion) == RSX_TG_OK);
    near(motion.x, -10);

    /* Current w = 1,2,4 gives perspective weights 1/2,1/4,1/4.
     * Current projected x = 0.5/2 = 0.25. Previous x = 1.5/1.25 = 1.2.
     * On 100x100 pixels this is +47.5 x and -20 y. */
    now[0] = (rsx_tg_clip){0, 0, 0.5f, 1};
    now[1] = (rsx_tg_clip){2, 0, 1, 2};
    now[2] = (rsx_tg_clip){0, 0, 2, 4};
    old[0] = (rsx_tg_clip){0, 0, 0.5f, 1};
    old[1] = (rsx_tg_clip){2, 0, 0.5f, 1};
    old[2] = (rsx_tg_clip){4, 2, 1, 2};
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) == RSX_TG_OK);
    near(motion.x, 47.5f); near(motion.y, -20);

    motion.x = 123; motion.y = 456;
    old[1].w = 0;
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) != RSX_TG_OK);
    near(motion.x, 123); near(motion.y, 456);
    old[1].w = -1;
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) != RSX_TG_OK);
    old[1].w = 1;
    old[1].z = -0.1f; /* Behind the previous near plane. */
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) != RSX_TG_OK);
    old[1].z = 0.5f;
    old[1].x = NAN;
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) != RSX_TG_OK);
    old[1].x = INFINITY;
    assert(rsx_tg_triangle_motion(now, old, bary, 100, 100,
                                  0, 0, 0, 0, &motion) != RSX_TG_OK);
    old[1].x = 2;
    {
        const float bad_bary[3] = {-0.01f, 0.51f, 0.5f};
        assert(rsx_tg_triangle_motion(now, old, bad_bary, 100, 100,
                                      0, 0, 0, 0, &motion) != RSX_TG_OK);
    }
    near(motion.x, 123); near(motion.y, 456);
}

static void test_history(void)
{
    rsx_tg_history *h = NULL;
    rsx_tg_match m;
    rsx_tg_clip bad[3] = {tri[0], tri[1], tri[2]};
    assert(rsx_tg_create(&h, 2, 6) == RSX_TG_OK);
    assert(h != NULL);
    assert(rsx_tg_begin_frame(h, 10, 100, 80, 1, -1, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 42, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_NO_PREVIOUS_FRAME);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 11, 100, 80, -2, 2, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 42, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_OK);
    assert(m.reason == RSX_TG_VALID && m.vertex_count == 3);
    assert(m.current[1].x == tri[1].x && m.previous[1].x == tri[1].x);
    near(m.current_jitter_x, -2); near(m.previous_jitter_y, -1);
    assert(rsx_tg_match_draw(h, 8, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_MISSING_DRAW);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 12, 100, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_TOPOLOGY_CHANGED);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 13, 100, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_TOPOLOGY_CHANGED);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 14, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_EXTENT_CHANGED);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 16, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_FRAME_DISCONTINUITY);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 17, 101, 80, 0, 0, 1) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_CAMERA_CUT);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);

    assert(rsx_tg_begin_frame(h, 18, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 2) == RSX_TG_DUPLICATE_ID);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_HISTORY_FAILED);
    assert(rsx_tg_end_frame(h) == RSX_TG_BAD_STATE);

    assert(rsx_tg_begin_frame(h, 19, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 0, 43, tri, 2) == RSX_TG_INVALID_ARGUMENT);
    assert(rsx_tg_end_frame(h) == RSX_TG_BAD_STATE);

    assert(rsx_tg_begin_frame(h, 20, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 8, 44, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 9, 45, tri, 1) == RSX_TG_CAPACITY);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_HISTORY_FAILED);
    assert(rsx_tg_end_frame(h) == RSX_TG_BAD_STATE);

    assert(rsx_tg_begin_frame(h, 21, 101, 80, 0, 0, 0) == RSX_TG_OK);
    bad[1].x = NAN;
    assert(rsx_tg_append_draw(h, 7, 43, bad, 3) == RSX_TG_INVALID_ARGUMENT);
    assert(rsx_tg_end_frame(h) == RSX_TG_BAD_STATE);

    rsx_tg_reset(h);
    assert(rsx_tg_begin_frame(h, 22, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 7, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 7, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_NO_PREVIOUS_FRAME);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);
    assert(rsx_tg_begin_frame(h, 23, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 8, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 8, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_MISSING_DRAW);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);
    assert(rsx_tg_begin_frame(h, 22, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 8, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 8, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_FRAME_DISCONTINUITY);
    /* Skipping end_frame makes this frame unusable as history. */
    assert(rsx_tg_begin_frame(h, 23, 101, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 8, 43, tri, 3) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 8, &m) == RSX_TG_UNAVAILABLE);
    assert(m.reason == RSX_TG_HISTORY_FAILED);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);
    rsx_tg_destroy(h);
    rsx_tg_destroy(NULL);
    h = NULL;
    assert(rsx_tg_create(&h, 1, 1) == RSX_TG_OK);
    /* Offscreen/behind-camera vertices are ordinary post-VS mesh data.
     * Retain them in history; the pure motion helper requires clipping. */
    const rsx_tg_clip behind = {0, 0, -2, -1};
    assert(rsx_tg_begin_frame(h, 1, 100, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 1, 1, &behind, 1) == RSX_TG_OK);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);
    assert(rsx_tg_begin_frame(h, 2, 100, 80, 0, 0, 0) == RSX_TG_OK);
    assert(rsx_tg_append_draw(h, 1, 1, &behind, 1) == RSX_TG_OK);
    assert(rsx_tg_match_draw(h, 1, &m) == RSX_TG_OK && m.previous[0].w == -1);
    assert(rsx_tg_end_frame(h) == RSX_TG_OK);
    rsx_tg_destroy(h);
}

int main(void)
{
    test_motion();
    test_history();
    puts("PASS temporal geometry: analytical motion, jitter, topology and history invalidation");
    return 0;
}
