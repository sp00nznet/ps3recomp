#include "rsx_scene_rgb.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *sha = "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634";
static const char *lf_sha = "4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7";
static int checks, failures;

#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

typedef struct fixture {
    float c[16][4];
    float v[12][4];
    int calls;
    int unavailable;
    int bad_coordinates;
    int bad_sample;
} fixture;

static int closef(float actual, float expected)
{
    return fabsf(actual - expected) < 0.00002f;
}

static int sample(void *context, unsigned unit, float u, float v, float lod,
                  float out[4])
{
    fixture *f = context;
    int call = f->calls++;
    const unsigned expected_unit[4] = {1, 1, 0, 2};
    const float expected_u[4] = {0.6f, 0.2f, 0.125f, 0.375f};
    const float expected_v[4] = {0.3f, 0.4f, 0.25f, 0.5f};
    const float expected_lod[4] = {1.25f, 1.25f, 0.5f, 2.5f};
    const float values[4][4] = {
        {0.25f, NAN, NAN, NAN},
        {0.75f, NAN, NAN, NAN},
        {0.5f, 1.0f, 0.25f, 0.8f},
        {2.0f, 3.0f, 4.0f, NAN}
    };
    if (call >= 4 || unit != expected_unit[call] ||
        !closef(u, expected_u[call]) || !closef(v, expected_v[call]) ||
        !closef(lod, expected_lod[call])) f->bad_coordinates = 1;
    if (f->unavailable == call + 1 || call >= 4) return 0;
    memcpy(out, values[call], sizeof(values[call]));
    if (f->bad_sample == call + 1) out[0] = NAN;
    return 1;
}

static void setup(fixture *f, rsx_scene_rgb_input *in)
{
    unsigned i;
    memset(f, 0, sizeof(*f));
    memset(in, 0, sizeof(*in));
    for (i = 0; i < 3; ++i) {
        f->c[0][i] = 1.0f;
        f->c[1][i] = 2.0f;
        f->c[6][i] = 1.0f;
        f->c[7][i] = 2.0f;
        f->c[8][i] = 0.5f;
        f->c[10][i] = 0.375f;
        f->c[12][i] = (float)(i + 1);
        f->c[13][i] = (float)(i + 4);
    }
    f->c[2][0] = 0.3f; f->c[3][0] = 0.4f;
    f->c[4][0] = 0.1f; f->c[5][0] = 0.2f;
    f->c[11][0] = 2.0f; f->c[14][0] = 0.5f;
    f->c[15][0] = 1.6f;
    f->v[1][0] = 0.5f; f->v[1][1] = 0.25f;
    f->v[1][2] = 1.0f; f->v[1][3] = 0.5f;
    f->v[2][0] = 1.0f; f->v[2][1] = 1.0f;
    f->v[2][2] = 0.25f; f->v[2][3] = 0.5f;
    f->v[4][0] = 0.125f; f->v[4][1] = 0.25f;
    f->v[5][2] = 0.375f; f->v[5][3] = 0.5f;
    f->v[6][0] = 1.0f; f->v[7][1] = 1.0f; f->v[11][0] = 1.0f;
    in->contract_id = 0;
    in->source_sha256 = sha;
    in->fp = (const float (*)[4])f->c;
    in->fp_bytes = sizeof(f->c);
    in->vertex = (const float (*)[4])f->v;
    in->vertex_bytes = sizeof(f->v);
    in->lod[0] = 0.5f; in->lod[1] = 1.25f; in->lod[2] = 2.5f;
    in->sample = sample;
    in->sample_context = f;
}

int main(void)
{
    fixture f;
    rsx_scene_rgb_input in;
    rsx_scene_rgb_output out;
    char uppercase[65];
    size_t i;
    setup(&f, &in);
    CHECK(rsx_scene_rgb_source_supported(0, sha));
    CHECK(rsx_scene_rgb_source_supported(0, lf_sha));
    memcpy(uppercase, sha, 65);
    for (i = 0; i < 64; ++i)
        if (uppercase[i] >= 'a' && uppercase[i] <= 'f') uppercase[i] -= 'a' - 'A';
    CHECK(rsx_scene_rgb_source_supported(0, uppercase));
    CHECK(!rsx_scene_rgb_source_supported(1, sha));
    uppercase[63] = 0;
    CHECK(!rsx_scene_rgb_source_supported(0, uppercase));
    CHECK(!rsx_scene_rgb_source_supported(0, "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d6340"));
    CHECK(!rsx_scene_rgb_source_supported(0, "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d63g"));

    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_OK);
    CHECK(out.valid && !out.keep && closef(out.raw_alpha, 1.6f) &&
          closef(out.clamped_coverage, 1.0f));
    CHECK(closef(out.base_rgb[0], 0.5f) && closef(out.base_rgb[1], 0.5f) &&
          closef(out.base_rgb[2], 0.5f));
    CHECK(closef(out.authored_rgb[0], 4.0f) && closef(out.authored_rgb[1], 6.0f) &&
          closef(out.authored_rgb[2], 8.0f));
    CHECK(f.calls == 4 && !f.bad_coordinates);
    f.c[15][0] = 1.59f; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_OK && out.keep);
    f.c[15][0] = 1.6f; f.v[2][0] = 0.0f; f.v[2][1] = 1.0f; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_OK &&
          closef(out.authored_rgb[0], 0.5f) && closef(out.authored_rgb[1], 1.0f) &&
          closef(out.authored_rgb[2], 1.5f));
    f.v[2][1] = 0.0f; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_OK &&
          closef(out.authored_rgb[0], 2.0f) && closef(out.authored_rgb[1], 2.5f) &&
          closef(out.authored_rgb[2], 3.0f));
    f.v[2][0] = 1.0f; f.v[2][1] = 1.0f;
    f.c[0][3] = NAN; f.v[0][0] = NAN; f.v[4][3] = NAN; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_OK);
    f.c[0][0] = NAN; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_NUMERIC_UNSUPPORTED &&
          !out.valid && isnan(out.authored_rgb[0]) && f.calls == 0);
    f.c[0][0] = 1.0f; f.v[6][0] = 0.0f;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_NUMERIC_UNSUPPORTED && !out.valid);
    f.v[6][0] = 1.0f; f.v[4][0] = NAN; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_NUMERIC_UNSUPPORTED &&
          !out.valid && f.calls == 0);
    f.v[4][0] = 0.125f; f.v[1][0] = 3.0e38f; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_NUMERIC_UNSUPPORTED && !out.valid);
    f.v[1][0] = 0.5f;
    in.fp_bytes = RSX_SCENE_RGB_FP_MIN_BYTES - 1;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_INVALID && !out.valid);
    in.fp_bytes = sizeof(f.c); f.unavailable = 2; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_SAMPLE_UNAVAILABLE && !out.valid);
    f.unavailable = 0; f.bad_sample = 4; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_NUMERIC_UNSUPPORTED && !out.valid);
    f.bad_sample = 0; in.contract_id = 7; f.calls = 0;
    CHECK(rsx_scene_rgb_evaluate(&in, &out) == RSX_SCENE_RGB_UNSUPPORTED && !out.valid && f.calls == 0);
    printf("scene RGB: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
