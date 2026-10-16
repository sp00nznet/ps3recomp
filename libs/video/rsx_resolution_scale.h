#ifndef FOLKLORE_RSX_RESOLUTION_SCALE_H
#define FOLKLORE_RSX_RESOLUTION_SCALE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lowers higher internal resolution into generated RSX fragment HLSL.
 *
 * PSInput.position.xy arrives as host SV_POSITION in physical raster pixels.
 * A block marked "RSX internal resolution" is inserted immediately after the
 * opening brace of main(PSInput input), converting xy back to guest raster
 * coordinates (x * guest_width / host_width, y * guest_height / host_height)
 * before the existing shader-window origin/pixel-center transform runs.
 * position.zw is never touched.
 *
 * capacity counts the terminating NUL. Returns -1 on invalid input or
 * insufficient capacity (buffer unchanged), 0 for identity scale (buffer
 * unchanged), 1 when the scale block was inserted.
 */
int rsx_fp_apply_resolution_scale(char* hlsl, uint32_t capacity,
                                  uint32_t guest_width, uint32_t guest_height,
                                  uint32_t host_width, uint32_t host_height);

#ifdef __cplusplus
}
#endif

#endif /* FOLKLORE_RSX_RESOLUTION_SCALE_H */
