#include "../rsx_hud_geometry.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); ++failures; \
} } while (0)

static int near(float a, float b)
{
    return fabsf(a - b) < 0.00001f;
}

static void test_parser(void)
{
    unsigned percent = 777;
    float scale = -1.0f;
    CHECK(rsx_hud_parse_percent("50", &percent) == 0 && percent == 50);
    CHECK(rsx_hud_parse_percent("75", &percent) == 0 && percent == 75);
    CHECK(rsx_hud_parse_scale("100", &scale) == 0 && near(scale, 1.0f));
    CHECK(rsx_hud_parse_scale("75", &scale) == 0 && near(scale, 0.75f));
    const char *invalid[] = { "", "0", "49", "101", "050", "1000",
                              "75.0", "+75", "-75", " 75", "75 ", "7e1", "nan" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        percent = 777; scale = -1.0f;
        CHECK(rsx_hud_parse_percent(invalid[i], &percent) == -1 && percent == 777);
        CHECK(rsx_hud_parse_scale(invalid[i], &scale) == -1 && scale == -1.0f);
    }
    percent = 777; scale = -1.0f;
    CHECK(rsx_hud_parse_percent(NULL, &percent) == -1 && percent == 777);
    CHECK(rsx_hud_parse_scale(NULL, &scale) == -1 && scale == -1.0f);
    CHECK(rsx_hud_parse_percent("75", NULL) == -1);
    CHECK(rsx_hud_parse_scale("75", NULL) == -1);
}

static void test_clip_transform(void)
{
    const float in[8] = { 1, 1, 0.4f, 0.5f, 0, 0, 0.6f, 0.7f };
    float out[8];
    CHECK(rsx_hud_compose_xf(in, 75, 0, 0, out) == 0);
    CHECK(near(out[0], 0.75f) && near(out[1], 0.75f));
    CHECK(near(out[4], -0.25f) && near(out[5], 0.25f));
    CHECK(out[2] == in[2] && out[3] == in[3] &&
          out[6] == in[6] && out[7] == in[7]);
    CHECK(rsx_hud_compose_xf(in, 75, 1, 0, out) == 0);
    CHECK(near(out[4], 0.25f) && near(out[5], 0.25f));
    CHECK(rsx_hud_compose_xf(in, 75, 1, 1, out) == 0);
    CHECK(near(out[4], 0.25f) && near(out[5], -0.25f));
    CHECK(rsx_hud_compose_xf(in, 100, 0, 0, out) == 0 &&
          memcmp(out, in, sizeof(out)) == 0);
    const float composed[8] = { 2, -3, 0.4f, 0.5f, 0.2f, -0.4f, 0.6f, 0.7f };
    CHECK(rsx_hud_compose_xf(composed, 75, 1, 1, out) == 0);
    CHECK(near(out[0], 1.5f) && near(out[1], -2.25f));
    CHECK(near(out[4], 0.4f) && near(out[5], -0.55f));
    CHECK(out[2] == composed[2] && out[3] == composed[3] &&
          out[6] == composed[6] && out[7] == composed[7]);

    for (unsigned i = 0; i < 8; ++i) out[i] = 9.0f;
    CHECK(rsx_hud_compose_xf(in, 49, 0, 0, out) == -1);
    CHECK(rsx_hud_compose_xf(in, 101, 0, 0, out) == -1);
    CHECK(rsx_hud_compose_xf(in, 75, 3, 0, out) == -1);
    float bad[8]; memcpy(bad, in, sizeof(bad)); bad[0] = NAN;
    CHECK(rsx_hud_compose_xf(bad, 75, 0, 0, out) == -1);
    for (unsigned i = 0; i < 8; ++i) CHECK(out[i] == 9.0f);
    CHECK(rsx_hud_compose_xf(in, 75, 0, 0, NULL) == -1);
}

static void test_scissor(void)
{
    rsx_hud_rect out;
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                100, 100, 75, 0, 0, &out) == 0);
    CHECK(out.x == 0 && out.y == 0 && out.w == 15 && out.h == 15);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                100, 100, 75, 1, 0, &out) == 0);
    CHECK(out.x == 25 && out.y == 0 && out.w == 15 && out.h == 15);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){80, 80, 20, 20},
                                100, 100, 75, 1, 1, &out) == 0);
    CHECK(out.x == 85 && out.y == 85 && out.w == 15 && out.h == 15);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){1, 1, 3, 3},
                                100, 100, 75, 0, 0, &out) == 0);
    CHECK(out.x == 0 && out.y == 0 && out.w == 3 && out.h == 3);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){90, 90, 30, 30},
                                100, 100, 75, 0, 0, &out) == 0);
    CHECK(out.x == 67 && out.y == 67 && out.w == 23 && out.h == 23);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){UINT32_MAX, UINT32_MAX, 20, 20},
                                100, 100, 75, 0, 0, &out) == 0);
    CHECK(out.x == 100 && out.y == 100 && out.w == 0 && out.h == 0);

    out = (rsx_hud_rect){7, 8, 9, 10};
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                0, 100, 75, 0, 0, &out) == -1);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                100, 0, 75, 0, 0, &out) == -1);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 0, 20},
                                100, 100, 75, 0, 0, &out) == -1);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                100, 100, 49, 0, 0, &out) == -1);
    CHECK(rsx_hud_scale_scissor((rsx_hud_rect){0, 0, 20, 20},
                                100, 100, 75, 0, 3, &out) == -1);
    CHECK(out.x == 7 && out.y == 8 && out.w == 9 && out.h == 10);
}

static void test_aspect_compensation(void)
{
    const float wide = (float)((16.0 / 9.0) / (3440.0 / 1440.0));
    const float superwide = (float)((16.0 / 9.0) / (5120.0 / 1440.0));
    const float in[8] = {1, 1, 0.4f, 0.5f, 0, 0, 0.6f, 0.7f};
    float old_xf[8], new_xf[8];
    rsx_hud_rect old_rect, new_rect;
    const rsx_hud_rect whole = {0, 0, 3440, 1440};

    CHECK(near(wide, 0.74418605f) && near(superwide, 0.5f));
    CHECK(rsx_hud_compose_xf(in, 75, 1, 1, old_xf) == 0);
    CHECK(rsx_hud_compose_xf_aspect(in, 75, 1.0f, 1, 1, new_xf) == 0);
    CHECK(memcmp(old_xf, new_xf, sizeof(old_xf)) == 0);
    CHECK(rsx_hud_scale_scissor(whole, 3440, 1440, 75, 1, 1, &old_rect) == 0);
    CHECK(rsx_hud_scale_scissor_aspect(whole, 3440, 1440, 75, 1.0f, 1, 1, &new_rect) == 0);
    CHECK(memcmp(&old_rect, &new_rect, sizeof(old_rect)) == 0);

    CHECK(rsx_hud_compose_xf_aspect(in, 75, wide, 0, 0, new_xf) == 0);
    CHECK(near(new_xf[0], 1920.0f / 3440.0f) && near(new_xf[1], 0.75f));
    CHECK(near(new_xf[4], -(1.0f - 1920.0f / 3440.0f)) && near(new_xf[5], 0.25f));
    CHECK(new_xf[2] == in[2] && new_xf[3] == in[3] &&
          new_xf[6] == in[6] && new_xf[7] == in[7]);
    CHECK(rsx_hud_compose_xf_aspect(in, 75, wide, 1, 1, new_xf) == 0);
    CHECK(near(new_xf[4], 1.0f - 1920.0f / 3440.0f) && near(new_xf[5], -0.25f));
    CHECK(rsx_hud_scale_scissor_aspect(whole, 3440, 1440, 75, wide, 0, 0, &new_rect) == 0);
    CHECK(new_rect.x == 0 && new_rect.y == 0 && new_rect.w == 1920 && new_rect.h == 1080);
    CHECK(rsx_hud_scale_scissor_aspect(whole, 3440, 1440, 75, wide, 1, 1, &new_rect) == 0);
    CHECK(new_rect.x == 1520 && new_rect.y == 360 && new_rect.w == 1920 && new_rect.h == 1080);

    CHECK(rsx_hud_compose_xf_aspect(in, 75, superwide, 0, 1, new_xf) == 0);
    CHECK(near(new_xf[0], 0.375f) && near(new_xf[1], 0.75f));
    CHECK(near(new_xf[4], -0.625f) && near(new_xf[5], -0.25f));
    CHECK(rsx_hud_scale_scissor_aspect((rsx_hud_rect){0,0,5120,1440},
                                       5120,1440,75,superwide,1,1,&new_rect) == 0);
    CHECK(new_rect.x == 3200 && new_rect.y == 360 && new_rect.w == 1920 && new_rect.h == 1080);
    CHECK(rsx_hud_scale_scissor_aspect((rsx_hud_rect){1,1,3,3},
                                       5120,1440,75,superwide,0,0,&new_rect) == 0);
    CHECK(new_rect.x == 0 && new_rect.y == 0 && new_rect.w == 2 && new_rect.h == 3);
    CHECK(rsx_hud_scale_scissor_aspect((rsx_hud_rect){UINT32_MAX,UINT32_MAX,20,20},
                                       5120,1440,75,superwide,0,0,&new_rect) == 0);
    CHECK(new_rect.x == 5120 && new_rect.y == 1440 && new_rect.w == 0 && new_rect.h == 0);

    const float invalid[] = {0.0f, -0.5f, 1.01f, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        for (unsigned j = 0; j < 8; ++j) new_xf[j] = 9.0f;
        new_rect = (rsx_hud_rect){7,8,9,10};
        CHECK(rsx_hud_compose_xf_aspect(in, 75, invalid[i], 0, 0, new_xf) == -1);
        CHECK(rsx_hud_scale_scissor_aspect(whole, 3440, 1440, 75,
                                           invalid[i], 0, 0, &new_rect) == -1);
        for (unsigned j = 0; j < 8; ++j) CHECK(new_xf[j] == 9.0f);
        CHECK(new_rect.x == 7 && new_rect.y == 8 && new_rect.w == 9 && new_rect.h == 10);
    }
    CHECK(rsx_hud_compose_xf_aspect(in, 75, wide, 0, 0, NULL) == -1);
    CHECK(rsx_hud_scale_scissor_aspect(whole, 0, 1440, 75, wide, 0, 0, &new_rect) == -1);
}

static void test_center_and_presentation(void)
{
    const float in[8] = {1, 1, 0, 0, 0, 0, 0, 0};
    const float wide = (float)((16.0 / 9.0) / (3440.0 / 1440.0));
    float out[8];
    rsx_hud_rect rect;
    CHECK(rsx_hud_anchor_clip(-0.2,0.2,0) == 2);
    CHECK(rsx_hud_anchor_clip(-0.9,0.9,0) == 2);
    CHECK(rsx_hud_anchor_clip(-0.95,-0.65,0) == 0);
    CHECK(rsx_hud_anchor_clip(0.65,0.95,0) == 1);
    CHECK(rsx_hud_anchor_clip(0.65,0.95,1) == 0);
    CHECK(rsx_hud_anchor_clip(-0.95,-0.65,1) == 1);
    CHECK(rsx_hud_compose_xf_aspect(in, 75, wide, 2, 2, out) == 0);
    CHECK(near(out[0], 1920.0f / 3440.0f) && near(out[1], 0.75f));
    CHECK(near(out[4], 0) && near(out[5], 0));
    CHECK(rsx_hud_scale_scissor_aspect((rsx_hud_rect){0,0,3440,1440},
        3440,1440,75,wide,2,2,&rect) == 0);
    CHECK(rect.x == 760 && rect.y == 180 && rect.w == 1920 && rect.h == 1080);
    CHECK(rsx_hud_compose_xf_aspect(in, 75, wide, 3, 2, out) == -1);

}

static void test_full_target(void)
{
    const rsx_hud_rect whole={0,0,1280,720};
    CHECK(rsx_hud_covers_target(1280,720,whole,whole,640,360,640,360));
    CHECK(rsx_hud_covers_target(1280,720,whole,whole,640,-360,640,360));
    CHECK(!rsx_hud_covers_target(1280,720,(rsx_hud_rect){1,0,1280,720},whole,640,360,640,360));
    CHECK(!rsx_hud_covers_target(1280,720,whole,(rsx_hud_rect){0,0,1279,720},640,360,640,360));
    CHECK(!rsx_hud_covers_target(1280,720,whole,whole,320,180,640,360));
    CHECK(!rsx_hud_covers_target(1280,720,whole,whole,640,360,641,360));
    CHECK(!rsx_hud_covers_target(1280,720,whole,whole,NAN,360,640,360));
    CHECK(!rsx_hud_covers_target(0,720,whole,whole,0,360,0,360));
}

int main(void)
{
    test_parser(); test_clip_transform(); test_scissor(); test_aspect_compensation();
    test_center_and_presentation(); test_full_target();
    printf("test_rsx_hud_geometry: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
