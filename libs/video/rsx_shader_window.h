/* NV4097 SET_SHADER_WINDOW fields and unit-resolution WPOS coefficients.
 * Host input coordinates use a top origin and half-pixel centers (D3D/Vulkan).
 * Attachment addressing and viewport geometry are separate state. */
#ifndef PS3RECOMP_RSX_SHADER_WINDOW_H
#define PS3RECOMP_RSX_SHADER_WINDOW_H
#include "ps3emu/ps3types.h"
typedef struct rsx_shader_window {
    float y_scale, x_bias, y_bias;
} rsx_shader_window;
static inline int rsx_shader_window_decode(u32 raw, rsx_shader_window* out)
{
    /* Height [0:11]; origin [12:15] and center [16:19] each accept 0 or 1. */
    if (!out || (raw & ~0x00011FFFu)) return -1;
    const int bottom = (raw & 0x1000u) != 0;
    const float center = (raw & 0x10000u) ? -0.5f : 0.0f;
    out->y_scale = bottom ? -1.0f : 1.0f;
    out->x_bias = center;
    out->y_bias = (bottom ? (float)(raw & 0xFFFu) : 0.0f) + center;
    return 0;
}
#endif
