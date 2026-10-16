#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define PS3RECOMP_RSX_CACAO
#include "rsx_draw_engine.c"

#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(cond) do { ++checks; if (!(cond)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
    return __LINE__; } } while (0)

/* NV40 texture format byte: base A8R8G8B8 plus the unnormalized flag. */
#define AOT_A8R8G8B8 0x85u
#define AOT_UN       0x40u
#define AOT_FMT      (AOT_A8R8G8B8 | AOT_UN)
#define AOT_BAD_FMT  ((rsx_be_format)(RSX_BE_FMT_R8G8B8A8 + 1))

#define AOT_OUT (&g.surfaces[0])
#define AOT_SRC (&g.surfaces[1])
#define AOT_DEP (&g.zdepths[0])

/* Synthetic fixture plus what the backend stub was handed. */
static struct {
    rsx_draw_backend be;
    rsx_be_render_state rs;
    rsx_dsp_texture tex[2];
    rsx_ao_projection proj;
    u32 targets[2], n_targets, mask, zslot, original;
    u32 calls, ret, color, depth, w, h, remap, fmt;
    float radius, strength, matrix[16];
    void* user;
} tf;

static u32 aot_record(void* user, u32 color, u32 depth, u32 w, u32 h,
                      const float projection[16], u32 remap, u32 rsx_format,
                      float radius, float strength)
{
    ++tf.calls;
    tf.user = user; tf.color = color; tf.depth = depth; tf.w = w; tf.h = h;
    tf.remap = remap; tf.fmt = rsx_format; tf.radius = radius; tf.strength = strength;
    if (projection) memcpy(tf.matrix, projection, sizeof(tf.matrix));
    return tf.ret;
}

/* Source and depth share samples; storage expands 2x wide for MSAA, 2x tall at 4x. */
static void aot_storage(u32 samples)
{
    const u32 w = 64u * (samples > 1 ? 2u : 1u), h = 32u * (samples == 4 ? 2u : 1u);
    AOT_SRC->samples = samples; AOT_DEP->samples = samples;
    AOT_SRC->w = w; AOT_DEP->w = w; AOT_DEP->used_w = w;
    AOT_SRC->h = h; AOT_DEP->h = h; AOT_DEP->used_h = h;
    AOT_SRC->pitch = w * 4u; AOT_DEP->pitch = w * 4u;
    for (int i = 0; i < 2; ++i) {
        tf.tex[i].width = w; tf.tex[i].height = h; tf.tex[i].pitch = w * 4u;
    }
}

static void aot_arm(eng_zdepth* z, u32 frame, u64 serial, u32 w, u32 h)
{
    rsx_ao_depth_clear(&z->ao, frame, 1.0f, serial);
    rsx_ao_depth_write(&z->ao, frame, serial, w, h, &tf.proj);
}

static void aot_block(eng_zdepth* z)
{
    rsx_ao_depth_clear(&z->ao, g.frames, 0.5f, z->depth_serial);
}

static void aot_fixture(void)
{
    memset(&g, 0, sizeof(g));
    memset(&tf, 0, sizeof(tf));
    tf.ret = 91u; tf.original = 77u; tf.mask = 3u; tf.n_targets = 1u;
    tf.targets[0] = 0u; tf.zslot = ENG_INVALID;
    tf.be.user = &tf; tf.be.ambient_occlusion = aot_record;
    tf.rs.color_mask = 0x01010101u;
    tf.proj.matrix[0][0] = 1.25f; tf.proj.matrix[1][1] = 2.5f;
    tf.proj.matrix[2][2] = -1.01f; tf.proj.matrix[3][3] = 0.75f;
    tf.proj.depth_a = 1.01f; tf.proj.depth_b = -0.1f;
    tf.proj.near_z = 0.1f; tf.proj.far_z = 10.0f;

    g.be = &tf.be; g.ao_enabled = 1; g.ao_radius = 1.2f; g.ao_strength = 0.75f;
    g.frames = 7u; g.ao_logged = 1;

    AOT_OUT->location = 0u; AOT_OUT->offset = 0x1000u;
    AOT_OUT->w = 64u; AOT_OUT->h = 32u; AOT_OUT->raster_w = 64u; AOT_OUT->raster_h = 32u;
    AOT_OUT->samples = 1u; AOT_OUT->pitch = 256u; AOT_OUT->fmt = RSX_BE_FMT_R8G8B8A8;
    AOT_OUT->handle = 10u;
    AOT_SRC->location = 0u; AOT_SRC->offset = 0x10000u;
    AOT_SRC->raster_w = 64u; AOT_SRC->raster_h = 32u; AOT_SRC->fmt = RSX_BE_FMT_R8G8B8A8;
    AOT_SRC->handle = 20u; AOT_SRC->color_serial = 12u;
    g.n_surfaces = 2u;

    AOT_DEP->location = 0u; AOT_DEP->offset = 0x30000u;
    AOT_DEP->raster_w = 64u; AOT_DEP->raster_h = 32u; AOT_DEP->handle = 30u;
    AOT_DEP->cleared = 1; AOT_DEP->had_write = 1; AOT_DEP->depth_serial = 11u;
    AOT_DEP->zfmt = 2u;
    g.n_zdepths = 1u;

    for (int i = 0; i < 2; ++i) {
        tf.tex[i].dimension = 2; tf.tex[i].enabled = 1; tf.tex[i].location = 0u;
        tf.tex[i].format = AOT_FMT; tf.tex[i].mipmaps = 1u;
    }
    tf.tex[0].offset = 0x10000u; tf.tex[0].remap = 0x00AA39E4u;
    tf.tex[1].offset = 0x30000u; tf.tex[1].remap = 0x000000E4u;
    aot_storage(2u);
    aot_arm(AOT_DEP, 7u, 11u, 64u, 32u);

    /* Registered scanout can be larger than the active output raster. */
    g.display_buffers[0].location = 0u; g.display_buffers[0].offset = 0x1000u;
    g.display_buffers[0].pitch = 5120u; g.display_buffers[0].width = 1280u;
    g.display_buffers[0].height = 720u; g.display_buffers[0].valid = 1;
}

static u32 aot_run(void)
{
    return eng_ao_composite(tf.targets, tf.n_targets, tf.zslot, &tf.rs, tf.mask,
                            &tf.tex[0], &tf.tex[1], tf.original);
}

static eng_surface* aot_color_alias(u32 offset, u32 handle, u64 serial)
{
    eng_surface* s = &g.surfaces[g.n_surfaces++];
    *s = *AOT_SRC; s->offset = offset; s->handle = handle; s->color_serial = serial;
    return s;
}

static eng_zdepth* aot_depth_alias(u32 offset, u32 handle, u64 serial)
{
    eng_zdepth* z = &g.zdepths[g.n_zdepths++];
    *z = *AOT_DEP; z->offset = offset; z->handle = handle; z->depth_serial = serial;
    return z;
}

#define REJECT(mutation) do { aot_fixture(); mutation; \
    CHECK(aot_run() == tf.original); CHECK(tf.calls == 0u); } while (0)
#define ACCEPT(mutation) do { aot_fixture(); mutation; \
    CHECK(aot_run() == 91u); CHECK(tf.calls == 1u); } while (0)

static int test_base(void)
{
    static unsigned char surfaces[sizeof(g.surfaces)], zdepths[sizeof(g.zdepths)];
    rsx_dsp_texture tex[2];
    aot_fixture();
    memcpy(surfaces, g.surfaces, sizeof(surfaces));
    memcpy(zdepths, g.zdepths, sizeof(zdepths));
    memcpy(tex, tf.tex, sizeof(tex));
    CHECK(aot_run() == 91u);
    CHECK(tf.calls == 1u);
    CHECK(tf.user == &tf);
    CHECK(tf.color == 20u && tf.depth == 30u);
    CHECK(tf.w == 64u && tf.h == 32u);
    CHECK(tf.remap == 0x39E4u);
    CHECK(tf.fmt == AOT_FMT);
    CHECK(tf.radius == 1.2f && tf.strength == 0.75f);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK(tf.matrix[r * 4 + c] == (float)AOT_DEP->ao.projection.matrix[r][c]);
    CHECK(memcmp(surfaces, g.surfaces, sizeof(surfaces)) == 0);
    CHECK(memcmp(zdepths, g.zdepths, sizeof(zdepths)) == 0);
    CHECK(memcmp(tex, tf.tex, sizeof(tex)) == 0);
    CHECK(AOT_SRC->color_serial == 12u && AOT_DEP->depth_serial == 11u);
    return 0;
}

static int test_samples(void)
{
    ACCEPT(aot_storage(1u));
    CHECK(tf.color == 20u && tf.w == 64u && tf.h == 32u);
    ACCEPT(aot_storage(4u));
    CHECK(tf.depth == 30u && tf.w == 64u && tf.h == 32u);
    REJECT(aot_storage(3u));
    REJECT(AOT_OUT->samples = 2u);
    REJECT({ AOT_DEP->samples = 4u; AOT_DEP->h = 64u; AOT_DEP->used_h = 64u; });
    REJECT({ AOT_SRC->samples = 1u; AOT_SRC->w = 64u; AOT_SRC->pitch = 256u;
             tf.tex[0].width = 64u; tf.tex[0].pitch = 256u; });
    return 0;
}

static int test_params_backend(void)
{
    REJECT(g.ao_enabled = 0);
    REJECT(g.be = NULL);
    REJECT(tf.be.ambient_occlusion = NULL);
    REJECT(tf.original = 0u);
    REJECT(g.ao_radius = 0.0f);
    REJECT(g.ao_radius = -1.0f);
    REJECT(g.ao_radius = 1000.5f);
    REJECT(g.ao_radius = NAN);
    REJECT(g.ao_radius = INFINITY);
    REJECT(g.ao_strength = 0.0f);
    REJECT(g.ao_strength = -0.5f);
    REJECT(g.ao_strength = 1.01f);
    REJECT(g.ao_strength = NAN);
    ACCEPT({ g.ao_radius = 1000.0f; g.ao_strength = 1.0f; });
    CHECK(tf.radius == 1000.0f && tf.strength == 1.0f);
    aot_fixture();
    tf.ret = 0u;
    CHECK(aot_run() == 77u);
    CHECK(tf.calls == 1u);
    return 0;
}

static int test_target_state(void)
{
    REJECT(tf.mask = 1u);
    REJECT(tf.mask = 2u);
    REJECT(tf.mask = 7u);
    REJECT(tf.n_targets = 0u);
    REJECT(tf.n_targets = 2u);
    REJECT(tf.targets[0] = ENG_INVALID);
    REJECT(tf.targets[0] = g.n_surfaces);
    REJECT(AOT_OUT->handle = 0u);
    REJECT(AOT_OUT->fmt = AOT_BAD_FMT);
    REJECT(g.display_buffers[0].valid = 0);
    REJECT(g.display_buffers[0].offset = 0x2000u);
    REJECT(g.display_buffers[0].location = 1u);
    ACCEPT({ g.display_buffers[3] = g.display_buffers[0];
             memset(&g.display_buffers[0], 0, sizeof(g.display_buffers[0])); });
    ACCEPT({ g.display_buffers[0].width = 1920u; g.display_buffers[0].height = 1080u;
             g.display_buffers[0].pitch = 7680u; });
    REJECT(tf.rs.blend_enable = 1u);
    REJECT(tf.rs.depth_test = 1u);
    REJECT(tf.rs.depth_write = 1u);
    REJECT(tf.rs.alpha_test_enable = 1u);
    REJECT(tf.rs.stencil_enable = 1u);
    REJECT(tf.rs.color_mask = 0x01010100u);
    REJECT(tf.rs.color_mask = 0u);
    return 0;
}

static int test_textures_dimensions(void)
{
    REJECT(AOT_SRC->ao_invalidated = 1);
    REJECT(tf.tex[0].dimension = 3);
    REJECT(tf.tex[1].dimension = 1);
    REJECT(tf.tex[0].depth = 2);
    REJECT(tf.tex[1].depth = 2);
    REJECT({ tf.tex[0].pitch = 0; AOT_SRC->pitch = 0; });
    REJECT({ tf.tex[1].pitch = 0; AOT_DEP->pitch = 0; });
    REJECT(tf.tex[0].enabled = 0);
    REJECT(tf.tex[1].enabled = 0);
    REJECT(tf.tex[0].cubemap = 1);
    REJECT(tf.tex[1].mipmaps = 2u);
    REJECT(tf.tex[0].mipmaps = 0u);
    REJECT(tf.tex[0].format = 0x81u | AOT_UN);
    REJECT(tf.tex[1].format = 0x86u | AOT_UN);
    REJECT(tf.tex[1].offset = 0x10000u);
    REJECT(tf.tex[0].offset = 0x1000u);
    REJECT(tf.tex[1].offset = 0x1000u);
    REJECT(tf.tex[0].width = 64u);
    REJECT(tf.tex[0].height = 64u);
    REJECT(tf.tex[0].pitch = 1024u);
    REJECT(tf.tex[1].width = 64u);
    REJECT(tf.tex[1].pitch = 1024u);
    REJECT(AOT_SRC->pitch = 1024u);
    REJECT(AOT_SRC->fmt = AOT_BAD_FMT);
    REJECT(AOT_DEP->pitch = 1024u);
    REJECT(AOT_DEP->w = 256u);
    REJECT({ AOT_OUT->raster_w = 128u; AOT_OUT->w = 128u; AOT_OUT->pitch = 512u; });
    REJECT({ AOT_SRC->raster_w = 32u; AOT_SRC->w = 64u; AOT_SRC->pitch = 256u;
             tf.tex[0].width = 64u; tf.tex[0].pitch = 256u; });
    REJECT(aot_arm(AOT_DEP, 7u, 11u, 32u, 32u));
    return 0;
}

static int test_depth_state(void)
{
    REJECT(rsx_ao_depth_write(&AOT_DEP->ao,7u,11u,64u,32u,NULL));
    ACCEPT(AOT_SRC->color_serial = 11u);
    REJECT(AOT_SRC->color_serial = 10u);
    REJECT(AOT_DEP->handle = 0u);
    REJECT(AOT_DEP->depth_serial = 0u);
    REJECT(AOT_DEP->depth_serial = 12u);
    REJECT(tf.zslot = 0u);
    REJECT(g.frames = 8u);
    REJECT(aot_arm(AOT_DEP, 6u, 11u, 64u, 32u));
    REJECT(rsx_ao_depth_clear(&AOT_DEP->ao, 7u, 1.0f, 11u));
    REJECT({ rsx_ao_depth_clear(&AOT_DEP->ao, 7u, 0.5f, 11u);
             rsx_ao_depth_write(&AOT_DEP->ao, 7u, 11u, 64u, 32u, &tf.proj); });
    REJECT({ rsx_ao_depth_clear(&AOT_DEP->ao, 7u, 1.0f, 11u);
             rsx_ao_depth_write(&AOT_DEP->ao, 6u, 11u, 64u, 32u, &tf.proj); });
    REJECT({ tf.proj.matrix[0][0] = 3.0f;
             rsx_ao_depth_write(&AOT_DEP->ao, 7u, 11u, 64u, 32u, &tf.proj); });
    ACCEPT(rsx_ao_depth_write(&AOT_DEP->ao, 7u, 11u, 64u, 32u, &tf.proj));
    return 0;
}

static int test_aliases(void)
{
    /* Colour at t0: the newest live match wins; equal-serial duplicates are ambiguous. */
    ACCEPT(aot_color_alias(0x10000u, 21u, 5u)->fmt = AOT_BAD_FMT);
    CHECK(tf.color == 20u);
    ACCEPT(aot_color_alias(0x10000u, 21u, 0u)->fmt = AOT_BAD_FMT);
    CHECK(tf.color == 20u);
    ACCEPT(aot_color_alias(0x10000u, 21u, 13u));
    CHECK(tf.color == 21u);
    REJECT(aot_color_alias(0x10000u, 21u, 13u)->fmt = AOT_BAD_FMT);
    REJECT(aot_color_alias(0x10000u, 21u, 12u));
    /* Depth at t0 newer than the colour means the colour was overwritten. */
    REJECT(aot_depth_alias(0x10000u, 40u, 13u));
    ACCEPT(aot_depth_alias(0x10000u, 40u, 5u));
    ACCEPT(aot_depth_alias(0x10000u, 40u, 0u));
    /* Colour at t1 at or after the depth serial means t1 is no longer depth. */
    REJECT(aot_color_alias(0x30000u, 50u, 11u));
    REJECT(aot_color_alias(0x30000u, 50u, 12u));
    ACCEPT(aot_color_alias(0x30000u, 50u, 10u));
    ACCEPT(aot_color_alias(0x30000u, 50u, 0u));
    /* Depth at t1: the newest live match is the one qualified. */
    ACCEPT(aot_block(aot_depth_alias(0x30000u, 31u, 5u)));
    CHECK(tf.depth == 30u);
    ACCEPT({ eng_zdepth* z = aot_depth_alias(0x30000u, 31u, 12u);
             aot_arm(z, 7u, 12u, 64u, 32u); });
    CHECK(tf.depth == 31u);
    REJECT(aot_block(aot_depth_alias(0x30000u, 31u, 12u)));
    REJECT(aot_depth_alias(0x30000u, 31u, 11u));
    return 0;
}

static int aot_env(const char* key,const char* value)
{
#ifdef _WIN32
    return _putenv_s(key,value);
#else
    return setenv(key,value,1);
#endif
}

static int test_configuration(void)
{
    aot_fixture();
    CHECK(aot_env("PS3RECOMP_RSX_AO","")==0);
    CHECK(aot_env("PS3RECOMP_RSX_AO_RADIUS","")==0);
    CHECK(aot_env("PS3RECOMP_RSX_AO_STRENGTH","")==0);
    eng_ao_configure();CHECK(!g.ao_enabled);
    CHECK(aot_env("PS3RECOMP_RSX_AO","cacao")==0);
    eng_ao_configure();CHECK(g.ao_enabled && g.ao_radius==1.2f && g.ao_strength==1.f);
    CHECK(aot_env("PS3RECOMP_RSX_AO_RADIUS","2.5")==0);
    CHECK(aot_env("PS3RECOMP_RSX_AO_STRENGTH","0.5")==0);
    eng_ao_configure();CHECK(g.ao_enabled && g.ao_radius==2.5f && g.ao_strength==.5f);
    const char* invalid[]={"nan","inf","-1","0","2junk","1001"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        CHECK(aot_env("PS3RECOMP_RSX_AO_RADIUS",invalid[i])==0);
        eng_ao_configure();CHECK(!g.ao_enabled);
    }
    CHECK(aot_env("PS3RECOMP_RSX_AO_RADIUS","")==0);
    CHECK(aot_env("PS3RECOMP_RSX_AO_STRENGTH","0")==0);
    eng_ao_configure();CHECK(!g.ao_enabled);
    CHECK(aot_env("PS3RECOMP_RSX_AO_STRENGTH","1.1")==0);
    eng_ao_configure();CHECK(!g.ao_enabled);
    CHECK(aot_env("PS3RECOMP_RSX_AO_STRENGTH","")==0);
    tf.be.ambient_occlusion=NULL;eng_ao_configure();CHECK(!g.ao_enabled);
    tf.be.ambient_occlusion=aot_record;
    CHECK(aot_env("PS3RECOMP_RSX_AO","unknown")==0);
    eng_ao_configure();CHECK(!g.ao_enabled);
    CHECK(aot_env("PS3RECOMP_RSX_AO","off")==0);
    eng_ao_configure();CHECK(!g.ao_enabled);
    return 0;
}

int main(void)
{
    static const struct { const char* name; int (*fn)(void); } tests[] = {
        { "base", test_base },
        { "samples", test_samples },
        { "params_backend", test_params_backend },
        { "target_state", test_target_state },
        { "textures_dimensions", test_textures_dimensions },
        { "depth_state", test_depth_state },
        { "aliases", test_aliases },
        { "configuration", test_configuration },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        const int line = tests[i].fn();
        if (line) {
            fprintf(stderr, "%s failed at line %d\n", tests[i].name, line);
            failed = 1;
        }
    }
    memset(&g, 0, sizeof(g));
    memset(&tf, 0, sizeof(tf));
    if (failed) return 1;
    printf("test_ao_composite_engine: %u checks passed\n", checks);
    return 0;
}

