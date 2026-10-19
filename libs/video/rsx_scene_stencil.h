/* Visibility qualification for ray geometry from a stencil-writing draw.
 * Only tests that pass independently of the stencil contents/reference are
 * accepted. Writes are performed by the original raster pipeline; later
 * content-dependent stencil tests remain unsupported by the scene consumer. */
#ifndef RSX_SCENE_STENCIL_H
#define RSX_SCENE_STENCIL_H
#include "rsx_draw_engine.h"

static inline int rsx_scene_stencil_face_unconditional(u32 function,u32 mask)
{
    if(function==0x207u)return 1; /* ALWAYS */
    /* The selected Vulkan depth formats have an eight-bit stencil aspect.
     * With a zero comparison mask both operands are exactly zero. */
    if((mask&0xffu)==0u)
        return function==0x202u || function==0x203u || function==0x206u;
    return 0;
}
static inline int rsx_scene_stencil_unconditional(const rsx_be_render_state* s)
{
    return !s->stencil_enable ||
        (rsx_scene_stencil_face_unconditional(s->s_func,s->s_func_mask) &&
         (!s->stencil_two_sided ||
          rsx_scene_stencil_face_unconditional(s->bs_func,s->s_func_mask)));
}
#endif
