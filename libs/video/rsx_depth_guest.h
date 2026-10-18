/* The value a host depth sample takes when the draw engine stores a depth
 * target into guest memory in its zeta layout (rsx_draw_engine.c
 * eng_zdepth_to_guest). Shared with the Vulkan backend's GPU conversion
 * (rsx_vulkan_depth_argb.inc) and its tests, which must reproduce it bit for
 * bit: two float roundings (no fused multiply-add; the runtime is built with
 * -ffp-contract=off), then truncation. The +0.5 rounds to even at 2^23 and up,
 * so d == 1.0f yields 0x1000000, whose low 24 bits are 0: that is what the
 * stored bytes have always been. */
#pragma once
#include <stdint.h>

static inline uint32_t rsx_depth_guest_clamp_bits(float d, float scale)
{
    if (!(d > 0.0f)) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    return (uint32_t)(d * scale + 0.5f);
}

/* Z24S8: the word whose bytes, big-endian, are [v >> 16, v >> 8, v, stencil 0]. */
static inline uint32_t rsx_depth_guest_z24(float d) { return rsx_depth_guest_clamp_bits(d, 16777215.0f); }
/* Z16: one big-endian half-word [v >> 8, v]. */
static inline uint32_t rsx_depth_guest_z16(float d) { return rsx_depth_guest_clamp_bits(d, 65535.0f); }
