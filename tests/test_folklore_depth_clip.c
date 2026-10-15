#include "../libs/video/rsx_commands.h"
#include "../libs/video/rsx_depth_clip.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; }
void ps3_ms(const char* key) { (void)key; }
static unsigned viewport_calls, draw_calls;
static float observed_near, observed_far;
static rsx_depth_clip observed_transform;
static u32 bits(float value) { u32 raw; memcpy(&raw, &value, sizeof raw); return raw; }
static float number(u32 raw) { float value; memcpy(&value, &raw, sizeof value); return value; }
static void viewport(void* user, const rsx_state* state)
{
    assert(user == &viewport_calls);
    ++viewport_calls;
    observed_near = state->clip_min;
    observed_far = state->clip_max;
    assert(rsx_depth_clip_build(state->clip_min, state->clip_max,
        state->viewport_scale[2], state->viewport_offset[2], &observed_transform) == 0);
}
static void draw(void* user, u32 primitive, u32 first, u32 count)
{
    assert(user == &viewport_calls && primitive == 5 && first == 0 && count == 3);
    assert(viewport_calls > 0);
    ++draw_calls;
}
static void write_float(rsx_state* state, u32 method, float value)
{ assert(rsx_process_method(state, method, bits(value)) == 0); }
static void reject(float n, float f, float scale, float offset)
{
    rsx_depth_clip value = { 31.0f, 37.0f, 41.0f, 43.0f }, original = value;
    assert(rsx_depth_clip_build(n, f, scale, offset, &value) == -1);
    assert(memcmp(&value, &original, sizeof value) == 0);
}
static void draw_triangle(rsx_state* state)
{
    assert(rsx_process_method(state, NV4097_SET_BEGIN_END, 5) == 0);
    assert(rsx_process_method(state, NV4097_DRAW_ARRAYS, 2u << 24) == 0);
    assert(rsx_process_method(state, NV4097_SET_BEGIN_END, 0) == 0);
}
int main(int argc, char** argv)
{
    rsx_state state; rsx_state_init(&state);
    rsx_backend backend = {0};
    backend.userdata = &viewport_calls; backend.set_viewport = viewport; backend.draw_arrays = draw;
    rsx_set_backend(&backend);
    assert(state.clip_min == 0 && state.clip_max == 1);
    assert(state.viewport_scale[2] == 0.5f && state.viewport_offset[2] == 0.5f);
    if (argc == 2) {
        const char* mode = argv[1];
        if (!strcmp(mode, "invalid-after-begin")) {
            assert(rsx_process_method(&state, NV4097_SET_BEGIN_END, 5) == 0);
            write_float(&state, NV4097_SET_CLIP_MIN, 1.0f);
            rsx_process_method(&state, NV4097_DRAW_ARRAYS, 2u << 24);
            return 99;
        }
        if (!strcmp(mode, "invalid-near")) write_float(&state, NV4097_SET_CLIP_MIN, -0.25f);
        else if (!strcmp(mode, "invalid-far")) write_float(&state, NV4097_SET_CLIP_MAX, 1.25f);
        else if (!strcmp(mode, "invalid-equal")) write_float(&state, NV4097_SET_CLIP_MIN, 1.0f);
        else if (!strcmp(mode, "invalid-reversed")) { write_float(&state, NV4097_SET_CLIP_MIN, 0.75f); write_float(&state, NV4097_SET_CLIP_MAX, 0.25f); }
        else if (!strcmp(mode, "invalid-nan")) write_float(&state, NV4097_SET_CLIP_MIN, number(0x7fc12345u));
        else if (!strcmp(mode, "invalid-infinity")) write_float(&state, NV4097_SET_CLIP_MAX, INFINITY);
        else if (!strcmp(mode, "invalid-tiny")) write_float(&state, NV4097_SET_CLIP_MAX, 0.000001f);
        else if (!strcmp(mode, "invalid-scale")) write_float(&state, 0x0a38, INFINITY);
        else if (!strcmp(mode, "invalid-offset")) write_float(&state, 0x0a28, NAN);
        else if (!strcmp(mode, "invalid-overflow")) { write_float(&state, NV4097_SET_CLIP_MAX, 0.5f); write_float(&state, 0x0a38, FLT_MAX); }
        else if (!strcmp(mode, "invalid-control")) assert(rsx_process_method(&state, NV4097_SET_ZMIN_MAX_CONTROL, 0) == 0);
        else return 2;
        rsx_process_method(&state, NV4097_SET_BEGIN_END, 5);
        return 99; /* PS3_STRICT_BOOT=1 must stop86 before backend callbacks. */
    }
    /* Literal expected values are derived independently, not by the helper. */
    static const struct { float n,f,s,o,z,w,hs,ho,hz,depth; int inside; } cases[] = {
        {0,1, .5f,.5f, -2,2, .5f,.5f, 0,0, 1},
        {0,1, .5f,.5f,  2,2, .5f,.5f, 2,1, 1},
        {.25f,.75f, .5f,.5f, -1,2, 1,.5f, 0,.25f, 1},
        {.25f,.75f, .5f,.5f, -.5f,2, 1,.5f, .5f,.375f, 1},
        {.25f,.75f, .5f,.5f, 1,2, 1,.5f, 2,.75f, 1},
        {.25f,.75f, .5f,.5f, -1.5f,2, 1,.5f, -.5f,.125f, 0},
        {.25f,.75f, .5f,.5f, 1.5f,2, 1,.5f, 2.5f,.875f, 0},
        {.25f,.75f, -.5f,.5f, .5f,2, -1,.5f, .5f,.375f, 1},
        {.25f,.75f, 0,.375f, 123,2, 0,.25f, .5f,.375f, 1},
        {.125f,.625f, .25f,.25f, 1,2, .5f,.25f, 1,.375f, 1}
    };
    for (unsigned i=0; i<sizeof cases/sizeof cases[0]; ++i) {
        rsx_depth_clip t;
        assert(rsx_depth_clip_build(cases[i].n,cases[i].f,cases[i].s,cases[i].o,&t)==0);
        assert(t.scale==cases[i].hs && t.offset==cases[i].ho);
        assert(t.min_depth==cases[i].n && t.max_depth==cases[i].f);
        float z = t.scale*cases[i].z+t.offset*cases[i].w;
        assert(z==cases[i].hz);
        assert((z>=0 && z<=cases[i].w)==cases[i].inside);
        assert(t.min_depth+(t.max_depth-t.min_depth)*(z/cases[i].w)==cases[i].depth);
    }
    reject(-.5f,1.5f,1,0); reject(.75f,.25f,.5f,.5f); reject(.5f,.5f,.5f,.5f);
    reject(0,.000001f,.5f,.5f); reject(0,0,.5f,.5f); reject(0,1.25f,.5f,.5f);
    reject(.25f,.75f,FLT_MAX,0); reject(.25f,.75f,0,FLT_MAX);
    const float exceptional[] = { NAN, INFINITY, -INFINITY };
    for (unsigned i=0;i<3;++i) {
        reject(exceptional[i],1,.5f,.5f); reject(0,exceptional[i],.5f,.5f);
        reject(0,1,exceptional[i],.5f); reject(0,1,.5f,exceptional[i]);
    }
    assert(rsx_depth_clip_build(0,1,.5f,.5f,NULL)==-1);
    rsx_depth_clip tiny;
    assert(rsx_depth_clip_build(0,.000002f,0,0,&tiny)==0 && tiny.scale==0 && tiny.offset==0);

    /* Raw float state survives the register write, including NaN payloads.
     * Validation belongs to submission, so a transient invalid pair is legal. */
    state.viewport_dirty=0;
    assert(rsx_process_method(&state,NV4097_SET_CLIP_MIN,0x7fc12345u)==0);
    assert(bits(state.clip_min)==0x7fc12345u && state.viewport_dirty);
    write_float(&state,NV4097_SET_CLIP_MIN,0.0f);
    write_float(&state,NV4097_SET_CLIP_MAX,.25f);
    write_float(&state,NV4097_SET_CLIP_MIN,.5f); /* temporarily reversed */
    assert(viewport_calls==0);
    write_float(&state,NV4097_SET_CLIP_MAX,.75f);
    draw_triangle(&state);
    assert(viewport_calls==1 && draw_calls==1 && !state.viewport_dirty);
    assert(observed_near==.5f && observed_far==.75f && observed_transform.scale==2 && observed_transform.offset==0);
    draw_triangle(&state);
    assert(viewport_calls==1 && draw_calls==2); /* unchanged state */
    write_float(&state,NV4097_SET_CLIP_MIN,.25f);
    draw_triangle(&state);
    assert(viewport_calls==2 && draw_calls==3 && observed_near==.25f && observed_far==.75f);
    assert(observed_transform.scale==1 && observed_transform.offset==.5f);
    write_float(&state,0x0a38,0); write_float(&state,0x0a28,.375f);
    draw_triangle(&state);
    assert(viewport_calls==3 && draw_calls==4 && observed_transform.scale==0 && observed_transform.offset==.25f);
    write_float(&state,NV4097_SET_CLIP_MIN,0); write_float(&state,NV4097_SET_CLIP_MAX,1);
    draw_triangle(&state);
    assert(viewport_calls==4 && draw_calls==5 && observed_near==0 && observed_far==1);
    puts("Depth clip math, raw registers, delayed validation and draw propagation passed");
    return 0;
}
